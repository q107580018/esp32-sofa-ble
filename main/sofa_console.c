#include "sofa_console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_console.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "sofa_ble.h"
#include "sofa_control.h"
#include "sofa_proto.h"
#include "wifi_sta.h"

static const char *TAG = "sofa-cli";

#define PROMPT "sofa> "

static void print_help(void)
{
    printf(
        "\n"
        "  sofa status                 link state, handles, counters\n"
        "  sofa list                   peripherals seen so far\n"
        "  sofa scan                   rescan (also reconnects if connected)\n"
        "  sofa connect <aa:bb:..>     connect to one address directly\n"
        "\n"
        "  sofa up   <1|2|3|4|all>     raise a seat\n"
        "  sofa down <1|2|3|4|all>     lower a seat\n"
        "  sofa stop [1|2|3|4|all]     stop; without a target, stop what is held\n"
        "\n"
        "  sofa mode oneshot|jog       movement mode (f1 00 / f1 01)\n"
        "  sofa dir  forward|reverse   movement direction (f2 00 / f2 01)\n"
        "  sofa handshake              send fe 5a again\n"
        "  sofa raw <hexbytes>         send arbitrary bytes, e.g. 'sofa raw fa 55'\n"
        "\n"
        "  In jog mode 'sofa up 1' keeps repeating until 'sofa stop'.\n"
        "  In one-shot mode the sofa runs to its end stop, or toggles off on a\n"
        "  second press - so 'sofa stop <seat>' only works while a seat is held.\n"
        "\n");
}

static void print_status(void)
{
    sofa_ble_info_t info;
    sofa_ble_get_info(&info);

    printf("link      : %s%s\n", sofa_state_name(info.state),
           info.connected ? ", connected" : "");
    printf("peer      : %s", info.peer_addr[0] ? info.peer_addr : "(none)");
    if (info.connected) {
        printf("  rssi %d dBm", info.rssi);
    }
    printf("\n");
    printf("service   : 0xae30 handles %u..%u\n", info.svc_start, info.svc_end);
    printf("write chr : 0xae01 value handle %u\n", info.chr_write_val);
    printf("notify chr: 0xae02 value handle %u, cccd %u\n", info.chr_notify_val,
           info.cccd_handle);
    printf("mtu       : %u\n", info.mtu);
    printf("movement  : %s\n", sofa_control_one_shot() ? "one-shot (一键启停)" : "jog (点动)");
    if (sofa_control_is_held()) {
        printf("holding   : %s\n", sofa_button_name(sofa_control_held_button()));
    }
    printf("tx/rx     : %u sent, %u dropped, %u notifications\n",
           (unsigned)info.tx_sent, (unsigned)info.tx_dropped,
           (unsigned)info.notify_count);

    if (strlen(CONFIG_SOFA_WIFI_SSID) == 0) {
        printf("network   : WiFi not configured, console only\n");
    } else if (wifi_sta_is_connected()) {
        char ip[16];
        wifi_sta_ip_str(ip, sizeof(ip));
#if CONFIG_SOFA_ENABLE_MDNS
        printf("network   : http://%s.local/  (http://%s/)  rssi %d dBm\n",
               CONFIG_SOFA_MDNS_HOSTNAME, ip, wifi_sta_rssi());
#else
        printf("network   : http://%s/  rssi %d dBm\n", ip, wifi_sta_rssi());
#endif
    } else {
        printf("network   : WiFi offline, retrying\n");
    }
}

static void print_list(void)
{
    sofa_seen_info_t seen[SOFA_SEEN_MAX];
    size_t n = sofa_ble_seen(seen, SOFA_SEEN_MAX);

    if (n == 0) {
        printf("nothing seen yet - 'sofa scan' and give it a few seconds\n");
        return;
    }

    printf("%-18s %-24s %5s  %s\n", "address", "name", "rssi", "");
    for (size_t i = 0; i < n; i++) {
        printf("%-18s %-24s %4d   %s\n", seen[i].addr,
               seen[i].name[0] ? seen[i].name : "?",
               seen[i].rssi,
               seen[i].matched ? "sofa" : "");
    }
}


static void report(const char *what, esp_err_t err)
{
    if (err == ESP_OK) {
        return;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        printf("%s: no ready link (see 'sofa status')\n", what);
    } else if (err == ESP_ERR_NOT_FOUND) {
        printf("%s: nothing has been commanded yet, so there is nothing to stop\n", what);
    } else if (err == ESP_ERR_TIMEOUT) {
        printf("%s: send queue full, try again\n", what);
    } else {
        printf("%s: %s\n", what, esp_err_to_name(err));
    }
}

/* Accepts "fe5a", "fa 01", "0xfa,0x01". Returns false on anything else. */
static bool parse_hex_bytes(const char *text, uint8_t *out, size_t *out_len)
{
    size_t n = 0;
    int hi = -1;

    for (const char *p = text; *p != '\0'; p++) {
        char c = *p;
        if (c == ' ' || c == ',' || c == '-' || c == ':') {
            continue;
        }
        int v;
        if (c >= '0' && c <= '9') {
            v = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v = c - 'A' + 10;
        } else if (c == 'x' || c == 'X') {
            continue; /* tolerate "0x" */
        } else {
            return false;
        }

        if (hi < 0) {
            hi = v;
        } else {
            if (n >= SOFA_TX_FRAME_MAX) {
                return false;
            }
            out[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }

    if (hi >= 0) {
        return false; /* odd number of hex digits */
    }
    *out_len = n;
    return n > 0;
}

static int cmd_sofa(int argc, char **argv)
{
    const char *sub = (argc >= 2) ? argv[1] : "help";

    if (strcasecmp(sub, "help") == 0 || strcmp(sub, "?") == 0) {
        print_help();
        return 0;
    }

    if (strcasecmp(sub, "status") == 0) {
        print_status();
        return 0;
    }

    if (strcasecmp(sub, "list") == 0) {
        print_list();
        return 0;
    }

    if (strcasecmp(sub, "scan") == 0) {
        sofa_ble_scan_restart();
        printf("rescanning\n");
        return 0;
    }

    if (strcasecmp(sub, "connect") == 0) {
        if (argc < 3) {
            printf("usage: sofa connect aa:bb:cc:dd:ee:ff\n");
            return 1;
        }
        esp_err_t err = sofa_ble_connect_to(argv[2]);
        if (err != ESP_OK) {
            report("connect", err);
            return 1;
        }
        printf("connecting to %s\n", argv[2]);
        return 0;
    }

    if (strcasecmp(sub, "up") == 0 || strcasecmp(sub, "down") == 0) {
        if (argc < 3) {
            printf("usage: sofa %s <1|2|3|4|all>\n", sub);
            return 1;
        }
        sofa_button_t up, down;
        if (!sofa_target_buttons(argv[2], &up, &down)) {
            printf("unrecognised seat '%s' (use 1..4 or all)\n", argv[2]);
            return 1;
        }
        esp_err_t err = sofa_control_move(strcasecmp(sub, "up") == 0 ? up : down);
        report("move", err);
        return err == ESP_OK ? 0 : 1;
    }

    if (strcasecmp(sub, "stop") == 0) {
        esp_err_t err;
        if (argc >= 3) {
            sofa_button_t up, down;
            if (!sofa_target_buttons(argv[2], &up, &down)) {
                printf("unrecognised seat '%s' (use 1..4 or all)\n", argv[2]);
                return 1;
            }
            err = sofa_control_stop_target(up, down);
            if (err == ESP_ERR_NOT_SUPPORTED) {
                printf("in one-shot mode the sofa stops on the *same* button it was "
                       "started with, so 'sofa stop %s' cannot know which one.\n"
                       "Use plain 'sofa stop' (stops whatever was pressed last), or "
                       "'sofa up %s' / 'sofa down %s' to toggle that one.\n",
                       argv[2], argv[2], argv[2]);
                return 1;
            }
        } else {
            err = sofa_control_stop();
        }
        report("stop", err);
        return err == ESP_OK ? 0 : 1;
    }

    if (strcasecmp(sub, "mode") == 0) {
        if (argc < 3) {
            printf("usage: sofa mode oneshot|jog\n");
            return 1;
        }
        bool one_shot;
        if (strcasecmp(argv[2], "oneshot") == 0 || strcasecmp(argv[2], "one-shot") == 0) {
            one_shot = true;
        } else if (strcasecmp(argv[2], "jog") == 0) {
            one_shot = false;
        } else {
            printf("usage: sofa mode oneshot|jog\n");
            return 1;
        }
        esp_err_t err = sofa_control_set_one_shot(one_shot);
        report("mode", err);
        return err == ESP_OK ? 0 : 1;
    }

    if (strcasecmp(sub, "dir") == 0) {
        if (argc < 3) {
            printf("usage: sofa dir forward|reverse\n");
            return 1;
        }
        bool forward;
        if (strcasecmp(argv[2], "forward") == 0 || strcasecmp(argv[2], "fwd") == 0) {
            forward = true;
        } else if (strcasecmp(argv[2], "reverse") == 0 || strcasecmp(argv[2], "rev") == 0) {
            forward = false;
        } else {
            printf("usage: sofa dir forward|reverse\n");
            return 1;
        }
        esp_err_t err = sofa_control_set_forward(forward);
        report("dir", err);
        return err == ESP_OK ? 0 : 1;
    }

    if (strcasecmp(sub, "handshake") == 0) {
        uint8_t frame[SOFA_FRAME_LEN];
        size_t len = sofa_frame_handshake(frame);
        esp_err_t err = sofa_control_send_raw(frame, len);
        report("handshake", err);
        return err == ESP_OK ? 0 : 1;
    }

    if (strcasecmp(sub, "raw") == 0) {
        if (argc < 3) {
            printf("usage: sofa raw <hexbytes>   e.g. sofa raw fa 55\n");
            return 1;
        }
        /* Re-join the remaining arguments so both "fa55" and "fa 55" work. */
        char joined[3 * SOFA_TX_FRAME_MAX];
        joined[0] = '\0';
        for (int i = 2; i < argc; i++) {
            if (i > 2) {
                strlcat(joined, " ", sizeof(joined));
            }
            strlcat(joined, argv[i], sizeof(joined));
        }

        uint8_t bytes[SOFA_TX_FRAME_MAX];
        size_t len = 0;
        if (!parse_hex_bytes(joined, bytes, &len)) {
            printf("could not parse '%s' as hex bytes (max %d bytes)\n", joined,
                   SOFA_TX_FRAME_MAX);
            return 1;
        }
        esp_err_t err = sofa_control_send_raw(bytes, len);
        report("raw", err);
        return err == ESP_OK ? 0 : 1;
    }

    printf("unknown subcommand '%s'\n", sub);
    print_help();
    return 1;
}

esp_err_t sofa_console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = PROMPT;
    repl_config.max_cmdline_length = 256;

    esp_console_register_help_command();

    const esp_console_cmd_t cmd = {
        .command = "sofa",
        .help = "Drive the sofa: 'sofa help' for the subcommand list",
        .func = &cmd_sofa,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));

    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_uart(&hw_config, &repl_config, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_console_new_repl_uart: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "console ready - type 'sofa help'");
    return esp_console_start_repl(repl);
}
