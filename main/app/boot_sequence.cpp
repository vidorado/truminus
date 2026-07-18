#include "boot_sequence.hpp"
#include "heapdiag.hpp"
#include "wifi_manager.hpp"
#include "c6_ota.hpp"
#include "p4display.hpp"
#include "victronble.hpp"
#include "ultimatronble.hpp"
#include "tankble.hpp"
#include "multiplusble.hpp"
#include "openairble.hpp"
#include "webserver.hpp"
#include "wstunnel.hpp"
#include "ws_broadcaster.hpp"
#include "cli.hpp"
#include "am2301.hpp"
#include "truma_lin.hpp"
#include "p4_ota.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_hosted_host_fw_ver.h"
extern "C" {
#include "esp_hosted_misc.h"
}

#define LIN_TX_PIN  27
#define LIN_RX_PIN  26
#define AM2301_DATA_PIN GPIO_NUM_52

static const char* TAG = "boot";

static volatile bool s_wifiAttempting = false;
static volatile bool s_linAttempting  = false;
static volatile bool s_bleAttempting  = false;

bool bootWifiAttempting() { return s_wifiAttempting; }
bool bootLinAttempting()  { return s_linAttempting;  }
bool bootBleAttempting()  {
    // Latch on first scan window (~15 s after boot) so the FAILED grace clock
    // in p4display doesn't start ticking before scanning actually begins.
    if (victronBleScanActive()) s_bleAttempting = true;
    return s_bleAttempting;
}

struct BootArgs {
    void (*wsCommandCb)(const char*, const char*);
    void (*wsConnectedCb)();
};
static BootArgs s_bootArgs;

// Background boot: everything that does not need to block the splash.
// Runs in parallel with the splash screen so the user sees pixels as fast
// as bsp_display_start_with_config() returns.  Each step is independent and
// already non-blocking (BLE supervisor self-spawns, wifi_manager_start is
// non-blocking, mountWebFs / startWebServer are fast).
static void bootTask(void* /*arg*/) {
    heapDiagMark("boot:start");

    // WiFi driver init.  ESP-Hosted transport to the C6 co-processor is
    // established here; the C6 OTA check that follows depends on it.
    wifi_manager_init();
    heapDiagMark("hosted+wifi_init");

    // C6 co-processor OTA: if the embedded slave firmware version differs
    // from the host ESP-Hosted library (major.minor), reflash the C6 via
    // SDIO and restart.  The OTA screen overrides the splash; the device
    // reboots when it completes, so we never return.
    {
        char slave_ver[16] = "?";
        if (c6OtaNeeded(slave_ver, sizeof(slave_ver))) {
            char host_ver[16];
            snprintf(host_ver, sizeof(host_ver), "%d.%d.%d",
                     ESP_HOSTED_VERSION_MAJOR_1,
                     ESP_HOSTED_VERSION_MINOR_1,
                     ESP_HOSTED_VERSION_PATCH_1);
            p4DisplayShowOtaScreen(slave_ver, host_ver);
            if (c6OtaPerform(p4DisplaySetOtaProgress)) {
                p4DisplaySetOtaProgress(100);
                vTaskDelay(pdMS_TO_TICKS(1500));
                esp_restart();
            }
        }
    }

    wifi_manager_start();
    s_wifiAttempting = true;
    heapDiagMark("wifi_start");

    // Initialize C6 BT controller via ESP-Hosted RPC before NimBLE starts.
    // This MUST complete before wstunnelInit(): the controller-init RPC and the
    // tunnel's TLS both ride the shared C6 hosted/SDIO transport, and running
    // them concurrently starves the tunnel's websocket client ("Could not lock
    // ws-client") and drops the connection.
    ESP_ERROR_CHECK(esp_hosted_bt_controller_init());
    ESP_ERROR_CHECK(esp_hosted_bt_controller_enable());
    heapDiagMark("bt_controller");

    victronBleInit();
    ultimatronBleInit();
    tankBleInit();
    multiplusBleInit();
    openairBleInit();
    heapDiagMark("ble_inits");
    // NimBLE init + supervisor task creation is deferred to the end of bootTask
    // (after boot:complete) so it does not interleave with the sequential
    // bring-up marks below — and so its heap peak does not overlap the web /
    // tunnel bring-up (consistent with the §16 "sequence, don't stack peaks"
    // rule). The supervisor task itself already waits for WiFi before scanning.

    // Web assets live on a LittleFS partition flashed from <project>/data/.
    if (mountWebFs() != ESP_OK) {
        ESP_LOGW(TAG, "LittleFS mount failed — run 'idf.py littlefs-flash-littlefs'");
    }
    heapDiagMark("littlefs");
    startWebServer(s_bootArgs.wsCommandCb, s_bootArgs.wsConnectedCb);
    heapDiagMark("webserver");

    // WebSocket reverse tunnel — exposes the local HTTP server through CGNAT
    // via a Plesk-hosted Node.js bridge.  Spawns its own task; respects the
    // "tunnel/enabled" NVS flag.  The TLS handshake runs at boot even on a
    // freshly-OTA'd image (PENDING_VERIFY): the L2-cache DRAM headroom keeps it
    // clear of the self-test floor, and bringing it up inside the window is
    // deliberate — the self-test then validates the tunnel too, and env_ready()
    // can fast-pass on a CONNECTED tunnel. See the firmware-ota skill.
    wstunnelInit();
    heapDiagMark("wstunnel_init");

    // WS pump runs at 100 ms cadence so touch inputs on the LCD reach
    // connected browsers in ≤100 ms.  Lower than the main loop's 1 s tick.
    xTaskCreate(wsPumpTask, "ws_pump", 4096, nullptr, 3, nullptr);
    heapDiagMark("ws_pump");

    // Serial REPL on USB-Serial-JTAG — provisions WiFi / Victron /
    // Ultimatron / tunnel credentials while the LCD settings screen is
    // unavailable.  Commands: `wifi`, `victron`, `ultimatron`, `tunnel`,
    // `show`, `help`.
    cliStart();
    heapDiagMark("cli");

    // AM2301 / DHT22 external temperature sensor on GPIO52 (RMT-based reader,
    // 30 s cadence).  Populates d.outdoorTemp in the main loop and broadcasts
    // as the WS `outdoor_temp` status id.
    am2301Start(AM2301_DATA_PIN);

    // LIN scheduler — emulates the CP-Plus D control unit on UART1.
    // Pinned to Core 0 (legacy convention: blocking serial off the LVGL core).
    trumaLinStart(LIN_TX_PIN, LIN_RX_PIN);
    s_linAttempting = true;
    heapDiagMark("am2301+lin");

    // Self-OTA: periodic GitHub release check + post-OTA self-test/rollback.
    // Spawned last so all the subsystems it watches (tunnel/LIN/BLE/web) are
    // already up before the self-test starts sampling their heartbeats.
    p4OtaStart();

    heapDiagMark("boot:complete");
    ESP_LOGI(TAG, "background boot complete (heap=%lu)",
             (unsigned long)esp_get_free_heap_size());

    // Bring up NimBLE + the BLE supervisor last, in its own task, so its heap
    // peak lands after every other subsystem has settled and its bring-up marks
    // do not interleave with bootTask's sequential marks above.
    xTaskCreate([](void*) {
        bleSupervisorStart();
        vTaskDelete(nullptr);
    }, "ble_start", 6144, nullptr, 1, nullptr);

    vTaskDelete(nullptr);
}

void bootStart(void (*wsCommandCb)(const char*, const char*), void (*wsConnectedCb)()) {
    s_bootArgs.wsCommandCb  = wsCommandCb;
    s_bootArgs.wsConnectedCb = wsConnectedCb;
    xTaskCreate(bootTask, "boot", 6144, nullptr, 5, nullptr);
}
