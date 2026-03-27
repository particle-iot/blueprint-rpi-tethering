/*

 * Project: B504E + M-Hat Cellular Tethering to Raspberry Pi
 * Author: Erik Fasnacht
 * Date: 3/23/2026
 * For comprehensive documentation and examples, please visit:
 * https://docs.particle.io/reference/device-os/tethering/
 */

#include "Particle.h"

SerialLogHandler logHandler(LOG_LEVEL_INFO);
SYSTEM_MODE(SEMI_AUTOMATIC);

#ifndef SYSTEM_VERSION_v620
    SYSTEM_THREAD(ENABLED);
#endif

// Tunable configuration
const uint32_t HEARTBEAT_INTERVAL_MS       = 5 * 60 * 1000;   // Heartbeat publish interval
const uint32_t STATUS_LOG_INTERVAL_MS      = 30 * 1000;        // Periodic status log interval
const uint32_t CONNECT_TIMEOUT_MS          = 90 * 1000;        // Warn if not connected after this
const uint32_t RECONNECT_RETRY_MS          = 30 * 1000;        // Time between reconnect attempts
const uint32_t ERROR_RETRY_DELAY_MS        = 60 * 1000;        // Back-off delay in ERROR_RECOVERY
const uint32_t FIRMWARE_UPDATE_MAX_TIME_MS = 5 * 60 * 1000;    // Max OTA wait before giving up
const int      MAX_RECONNECT_ATTEMPTS      = 3;                 // Failures before ERROR_RECOVERY

bool firmwareUpdateInProgress = false;


// TetherStateMachine — monitors the connection lifecycle (non-blocking)
class TetherStateMachine {
public:
    enum class AppState {
        CONNECTING,             // Waiting for cellular + Particle Cloud
        TETHERING_ACTIVE,       // All connected — RPi has cellular internet
        TETHER_RECONNECTING,    // Connection dropped — attempting to restore
        ERROR_RECOVERY,         // Max retries reached — waiting before retry
        FIRMWARE_UPDATE         // OTA update in progress — tethering continues
    };

private:
    AppState currentState  = AppState::CONNECTING;
    AppState previousState = AppState::CONNECTING;
    uint32_t stateEntryTime = 0;
    uint32_t bootTime       = 0;
    int      reconnectAttempts = 0;

public:
    void begin() { bootTime = millis(); setState(AppState::CONNECTING); }

    void setState(AppState newState) {
        if (currentState != newState) {
            previousState  = currentState;
            currentState   = newState;
            stateEntryTime = millis();
            Log.info("State: %s -> %s", toString(previousState), toString(currentState));
        }
    }

    AppState getState()           const { return currentState; }
    uint32_t timeInState()        const { return millis() - stateEntryTime; }
    uint32_t getTimeSinceBoot()   const { return millis() - bootTime; }

    void incrementReconnectAttempts() { reconnectAttempts++; }
    void resetReconnectAttempts()     { reconnectAttempts = 0; }
    int  getReconnectAttempts()  const { return reconnectAttempts; }

    bool shouldRetry() const {
        return (currentState == AppState::ERROR_RECOVERY) && (timeInState() > ERROR_RETRY_DELAY_MS);
    }

    const char* toString(AppState s) const {
        switch (s) {
            case AppState::CONNECTING:           return "CONNECTING";
            case AppState::TETHERING_ACTIVE:     return "TETHERING_ACTIVE";
            case AppState::TETHER_RECONNECTING:  return "TETHER_RECONNECTING";
            case AppState::ERROR_RECOVERY:       return "ERROR_RECOVERY";
            case AppState::FIRMWARE_UPDATE:      return "FIRMWARE_UPDATE";
            default:                             return "UNKNOWN";
        }
    }

    const char* currentStateStr() const { return toString(currentState); }
};

TetherStateMachine appStateMachine;

String tetherStatus = "connecting";
int    cellularRSSI = 0;

void updateStateMachine();
int  reconnectTether(String args);
void firmwareUpdateHandler(system_event_t event, int param);


void setup() {
    appStateMachine.begin();
    System.on(firmware_update, firmwareUpdateHandler);

    Particle.function("reconnect", reconnectTether);

    // Enable the M-Hat UART path to the RPi (isolated by default — see M-Hat datasheet)
    pinMode(A0, OUTPUT);
    digitalWrite(A0, HIGH);

    // Bind tether to Serial1 @ 921600 baud with RTS/CTS flow control
    Tether.bind(TetherSerialConfig().baudrate(921600).serial(Serial1));
    
    // Without flow control: 
    // Tether.bind(TetherSerialConfig().baudrate(921600).config(SERIAL_8N1).serial(Serial1));

    // turn on cellular and connect to Particle Cloud before starting the tethering state machine
    Cellular.on();
    Cellular.connect();
    Particle.connect();  // For OTA, remote management, and event publishing

    Log.info("Tethering firmware initialized");
}


void loop() {
    updateStateMachine();
    // Add application code here: sensors, watchdog, sleep management, etc.
}


// Force a reconnect from the Particle Console or API
int reconnectTether(String args) {
    if (appStateMachine.getState() == TetherStateMachine::AppState::TETHER_RECONNECTING) {
        return -1;  // Already reconnecting
    }
    Log.info("Remote reconnect requested");
    Tether.disconnect();
    Cellular.disconnect();
    appStateMachine.resetReconnectAttempts();
    appStateMachine.setState(TetherStateMachine::AppState::TETHER_RECONNECTING);
    return 0;
}


// OTA event handler — tethering continues during updates
void firmwareUpdateHandler(system_event_t event, int param) {
    switch (param) {
        case firmware_update_begin:
            firmwareUpdateInProgress = true;
            Log.info("OTA update started — tethering remains active");
            appStateMachine.setState(TetherStateMachine::AppState::FIRMWARE_UPDATE);
            break;
        case firmware_update_complete:
            firmwareUpdateInProgress = false;
            Log.info("OTA update complete — device will reset");
            break;
        case (int)firmware_update_failed:
            firmwareUpdateInProgress = false;
            Log.error("OTA update failed — returning to tethering");
            appStateMachine.setState(TetherStateMachine::AppState::TETHERING_ACTIVE);
            break;
    }
}


// State machine — rate-limited to once per second
void updateStateMachine() {
    static uint32_t lastUpdate = 0;
    if (millis() - lastUpdate < 1000) return;
    lastUpdate = millis();

    switch (appStateMachine.getState()) {

        case TetherStateMachine::AppState::CONNECTING:
            if (Cellular.ready() && Particle.connected()) {
                Tether.on();
                Tether.connect();
                tetherStatus = "active";
                Particle.publish("tether/connected", PRIVATE);
                appStateMachine.resetReconnectAttempts();
                appStateMachine.setState(TetherStateMachine::AppState::TETHERING_ACTIVE);
            } else if (appStateMachine.timeInState() > CONNECT_TIMEOUT_MS) {
                static bool warned = false;
                if (!warned) { Log.warn("Still connecting after %lus", CONNECT_TIMEOUT_MS / 1000); warned = true; }
            }
            break;

        case TetherStateMachine::AppState::TETHERING_ACTIVE: {
            CellularSignal sig = Cellular.RSSI();
            cellularRSSI = (int)sig.getStrength();
            tetherStatus = "active";

            static uint32_t lastHeartbeat = 0;
            if (millis() - lastHeartbeat > HEARTBEAT_INTERVAL_MS) {
                String payload = String::format("{\"signal\":%d,\"uptime\":%lu,\"state\":\"%s\"}", cellularRSSI, appStateMachine.getTimeSinceBoot() / 1000, appStateMachine.currentStateStr());
                Particle.publish("tether/heartbeat", payload, PRIVATE);
                lastHeartbeat = millis();
            }

            if (!Cellular.ready()) {
                Log.warn("Cellular lost — reconnecting");
                tetherStatus = "reconnecting";
                appStateMachine.setState(TetherStateMachine::AppState::TETHER_RECONNECTING);
            }
            break;
        }

        case TetherStateMachine::AppState::TETHER_RECONNECTING: {
            tetherStatus = "reconnecting";

            if (Cellular.ready() && Particle.connected()) {
                Log.info("Reconnected after %d attempt(s)", appStateMachine.getReconnectAttempts() + 1);
                appStateMachine.resetReconnectAttempts();
                Tether.on();
                Tether.connect();
                appStateMachine.setState(TetherStateMachine::AppState::TETHERING_ACTIVE);
                break;
            }

            static uint32_t lastRetry = 0;
            if (millis() - lastRetry > RECONNECT_RETRY_MS) {
                lastRetry = millis();
                appStateMachine.incrementReconnectAttempts();
                Log.warn("Reconnect attempt %d/%d", appStateMachine.getReconnectAttempts(), MAX_RECONNECT_ATTEMPTS);

                if (appStateMachine.getReconnectAttempts() >= MAX_RECONNECT_ATTEMPTS) {
                    Log.error("Max reconnect attempts reached — backing off");
                    tetherStatus = "error";
                    appStateMachine.setState(TetherStateMachine::AppState::ERROR_RECOVERY);
                } else {
                    Cellular.connect();
                    Particle.connect();
                }
            }
            break;
        }

        case TetherStateMachine::AppState::ERROR_RECOVERY:
            tetherStatus = "error";
            if (appStateMachine.shouldRetry()) {
                Log.info("Retrying after back-off");
                appStateMachine.resetReconnectAttempts();
                Cellular.connect();
                Particle.connect();
                appStateMachine.setState(TetherStateMachine::AppState::TETHER_RECONNECTING);
            }
            break;

        case TetherStateMachine::AppState::FIRMWARE_UPDATE: {
            tetherStatus = "updating";

            static uint32_t otaStart = 0;
            if (otaStart == 0) { otaStart = millis(); }

            if (!firmwareUpdateInProgress) {
                Log.info("OTA finished — awaiting reset");
                otaStart = 0;
            } else if (millis() - otaStart >= FIRMWARE_UPDATE_MAX_TIME_MS) {
                Log.warn("OTA timeout — returning to tethering");
                firmwareUpdateInProgress = false;
                otaStart = 0;
                appStateMachine.setState(TetherStateMachine::AppState::TETHERING_ACTIVE);
            }
            break;
        }
    }

    // Periodic status log
    static uint32_t lastLog = 0;
    if (millis() - lastLog > STATUS_LOG_INTERVAL_MS) {
        Log.info("State: %s | Cloud: %s | Cellular: %s | Signal: %d%% | Uptime: %lus",
                 appStateMachine.currentStateStr(),
                 Particle.connected() ? "connected" : "disconnected",
                 Cellular.ready()     ? "ready"     : "not ready",
                 cellularRSSI,
                 appStateMachine.getTimeSinceBoot() / 1000);
        lastLog = millis();
    }
}