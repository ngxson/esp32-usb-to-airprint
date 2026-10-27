// Improv Wi-Fi serial (components/improv) on the host.
//   improv_host selftest   packet format, parsing amid log text, every RPC
//   improv_host serve      an emulated board on stdin/stdout, for the test
//                          with the installer's own client library
//                          (flasher/tests/improv.test.mjs): log lines between
//                          the packets, networks to scan, a password to get right
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "improv.h"

static int failures;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                \
        }                                                              \
    } while (0)

// the emulated board
#define GOOD_SSID "HomeNet"
#define GOOD_PASSWORD "correct horse"
static bool s_online;
static int s_connects;

static bool fake_url(char *buf, size_t n, void *ctx)
{
    snprintf(buf, n, "%s", s_online ? "http://192.168.1.20/" : "");
    return s_online;
}

static bool fake_connect(const char *ssid, const char *password, void *ctx)
{
    s_connects++;
    s_online = !strcmp(ssid, GOOD_SSID) && !strcmp(password, GOOD_PASSWORD);
    return s_online;
}

// a change that fails, the board going back to the network it was on
static bool fail_keep(const char *ssid, const char *password, void *ctx)
{
    return false;
}

static const improv_network_t NETWORKS[] = {
    {"Cafe", -81, false},
    {GOOD_SSID, -70, true},
    {"", -40, true},            // hidden: not listed
    {GOOD_SSID, -48, true},     // a second access point: listed once, the strongest
    {"x", -9, false},           // a result 10 bytes long (see improv_result)
};
#define NUM_NETWORKS (int)(sizeof(NETWORKS) / sizeof(NETWORKS[0]))

static int fake_scan(improv_network_t *out, int max, void *ctx)
{
    int n = NUM_NETWORKS < max ? NUM_NETWORKS : max;
    memcpy(out, NETWORKS, (size_t)n * sizeof(*out));
    return n;
}

// ---- selftest: replies collected and parsed back

static uint8_t s_out[8192];
static size_t s_out_len;

static void capture(const uint8_t *data, size_t len, void *ctx)
{
    CHECK(s_out_len + len <= sizeof(s_out));
    if (s_out_len + len <= sizeof(s_out)) {
        memcpy(s_out + s_out_len, data, len);
        s_out_len += len;
    }
}

typedef struct {
    uint8_t type;
    uint8_t data[256];
    size_t len;
} packet_t;

// splits the captured output into packets, checking each one's framing
static int packets(packet_t *p, int max)
{
    int n = 0;
    size_t i = 0;
    while (i < s_out_len && n < max) {
        CHECK(s_out_len - i >= 11 && !memcmp(s_out + i, "IMPROV\x01", 7));
        if (s_out_len - i < 11)
            break;
        size_t len = s_out[i + 8];
        uint8_t sum = 0;
        for (size_t k = 0; k < 9 + len; k++)
            sum += s_out[i + k];
        CHECK(sum == s_out[i + 9 + len]);
        CHECK(s_out[i + 10 + len] == '\n');
        CHECK(len != '\n');
        p[n].type = s_out[i + 7];
        p[n].len = len;
        memcpy(p[n].data, s_out + i + 9, len);
        n++;
        i += 11 + len;
    }
    s_out_len = 0;
    return n;
}

// the strings of an RPC result
static int strings(const packet_t *p, char out[][64], int max)
{
    int n = 0;
    size_t i = 2;
    CHECK(p->type == IMPROV_TYPE_RPC_RESULT && p->data[1] == p->len - 2);
    while (i < p->len && n < max) {
        size_t l = p->data[i];
        snprintf(out[n++], 64, "%.*s", (int)l, (const char *)p->data + i + 1);
        i += 1 + l;
    }
    return n;
}

static size_t rpc(uint8_t *out, uint8_t cmd, const uint8_t *data, size_t n)
{
    uint8_t d[255];
    d[0] = cmd;
    d[1] = (uint8_t)n;
    if (n)
        memcpy(d + 2, data, n);
    return improv_packet(out, IMPROV_TYPE_RPC, d, n + 2);
}

static size_t wifi_rpc(uint8_t *out, const char *ssid, const char *password)
{
    uint8_t d[200];
    size_t sl = strlen(ssid), pl = strlen(password);
    d[0] = (uint8_t)sl;
    memcpy(d + 1, ssid, sl);
    d[1 + sl] = (uint8_t)pl;
    memcpy(d + 2 + sl, password, pl);
    return rpc(out, IMPROV_CMD_WIFI_SETTINGS, d, 2 + sl + pl);
}

static void selftest(void)
{
    improv_t im;
    improv_config_t cfg = {
        .firmware = "ESP32 USB to AirPrint",
        .version = "0.2.0",
        .chip = "ESP32-S3",
        .name = "Canon MG2500 (ESP32)",
        .write = capture,
        .url = fake_url,
        .connect = fake_connect,
        .scan = fake_scan,
    };
    improv_init(&im, &cfg);
    packet_t p[16];
    char s[8][64];
    uint8_t pkt[IMPROV_PACKET_MAX];

    // the client's "current state" request, byte for byte as the SDK writes it
    static const uint8_t state_req[11] = {'I', 'M', 'P', 'R', 'O', 'V', 1, 3, 2, 2, 0};
    uint8_t sum = 0;
    for (size_t i = 0; i < 11; i++)
        sum += state_req[i];
    CHECK(rpc(pkt, IMPROV_CMD_GET_CURRENT_STATE, NULL, 0) == 13 && !memcmp(pkt, state_req, 11) &&
          pkt[11] == sum && pkt[12] == '\n');

    // amid log output, false starts, split anywhere
    const char *noise = "I (812) main: ready\nIMPRO\nIIMPROV\x02junk IMP";
    improv_feed(&im, (const uint8_t *)noise, strlen(noise));
    size_t n = rpc(pkt, IMPROV_CMD_GET_DEVICE_INFO, NULL, 0);
    for (size_t i = 0; i < n; i++)
        improv_feed(&im, pkt + i, 1);
    CHECK(packets(p, 16) == 1);
    CHECK(strings(&p[0], s, 8) == 4 && !strcmp(s[0], "ESP32 USB to AirPrint") && !strcmp(s[1], "0.2.0") &&
          !strcmp(s[2], "ESP32-S3") && !strcmp(s[3], "Canon MG2500 (ESP32)"));
    CHECK(p[0].data[0] == IMPROV_CMD_GET_DEVICE_INFO);

    // not on a network: the state only
    improv_feed(&im, pkt, rpc(pkt, IMPROV_CMD_GET_CURRENT_STATE, NULL, 0));
    CHECK(packets(p, 16) == 1 && p[0].type == IMPROV_TYPE_CURRENT_STATE && p[0].data[0] == IMPROV_STATE_READY);

    // damaged packets and unknown commands
    n = rpc(pkt, IMPROV_CMD_GET_DEVICE_INFO, NULL, 0);
    pkt[n - 2] ^= 1;
    improv_feed(&im, pkt, n);
    CHECK(packets(p, 16) == 1 && p[0].type == IMPROV_TYPE_ERROR_STATE && p[0].data[0] == IMPROV_ERROR_INVALID_RPC);
    improv_feed(&im, pkt, rpc(pkt, 0x42, NULL, 0));
    CHECK(packets(p, 16) == 1 && p[0].type == IMPROV_TYPE_ERROR_STATE && p[0].data[0] == IMPROV_ERROR_UNKNOWN_RPC);
    improv_feed(&im, pkt, rpc(pkt, IMPROV_CMD_WIFI_SETTINGS, (const uint8_t *)"\x05" "ab", 3));
    CHECK(packets(p, 16) == 1 && p[0].type == IMPROV_TYPE_ERROR_STATE && p[0].data[0] == IMPROV_ERROR_INVALID_RPC);
    CHECK(s_connects == 0);

    // the networks: strongest first, once per name, no hidden one, then an empty result
    improv_feed(&im, pkt, rpc(pkt, IMPROV_CMD_GET_WIFI_NETWORKS, NULL, 0));
    CHECK(packets(p, 16) == 4);
    // 10 bytes long, padded with an empty string (see improv_result)
    CHECK(strings(&p[0], s, 8) == 4 && !strcmp(s[0], "x") && !strcmp(s[1], "-9") && !strcmp(s[2], "NO") && !s[3][0]);
    CHECK(p[0].len == 11);
    CHECK(strings(&p[1], s, 8) == 3 && !strcmp(s[0], GOOD_SSID) && !strcmp(s[1], "-48") && !strcmp(s[2], "YES"));
    CHECK(strings(&p[2], s, 8) == 3 && !strcmp(s[0], "Cafe") && !strcmp(s[1], "-81") && !strcmp(s[2], "NO"));
    CHECK(strings(&p[3], s, 8) == 0 && p[3].data[0] == IMPROV_CMD_GET_WIFI_NETWORKS);

    // a wrong password: provisioning, then unable to connect, still not provisioned
    improv_feed(&im, pkt, wifi_rpc(pkt, GOOD_SSID, "wrong"));
    CHECK(packets(p, 16) == 4);
    CHECK(p[0].type == IMPROV_TYPE_ERROR_STATE && p[0].data[0] == IMPROV_ERROR_NONE);
    CHECK(p[1].type == IMPROV_TYPE_CURRENT_STATE && p[1].data[0] == IMPROV_STATE_PROVISIONING);
    CHECK(p[2].type == IMPROV_TYPE_ERROR_STATE && p[2].data[0] == IMPROV_ERROR_UNABLE_TO_CONNECT);
    CHECK(p[3].type == IMPROV_TYPE_CURRENT_STATE && p[3].data[0] == IMPROV_STATE_READY);

    // the right one: provisioned, and the page's address
    improv_feed(&im, pkt, wifi_rpc(pkt, GOOD_SSID, GOOD_PASSWORD));
    CHECK(packets(p, 16) == 4);
    CHECK(p[2].type == IMPROV_TYPE_CURRENT_STATE && p[2].data[0] == IMPROV_STATE_PROVISIONED);
    CHECK(strings(&p[3], s, 8) == 1 && !strcmp(s[0], "http://192.168.1.20/") &&
          p[3].data[0] == IMPROV_CMD_WIFI_SETTINGS);
    CHECK(s_connects == 2);

    // from now on the state comes with the address
    improv_feed(&im, pkt, rpc(pkt, IMPROV_CMD_GET_CURRENT_STATE, NULL, 0));
    CHECK(packets(p, 16) == 2 && p[0].data[0] == IMPROV_STATE_PROVISIONED);
    CHECK(strings(&p[1], s, 8) == 1 && p[1].data[0] == IMPROV_CMD_GET_CURRENT_STATE);

    // a failed change, the board back on the previous network: "ready" so that the
    // installer shows the error, then the state asked again says provisioned
    im.cfg.connect = fail_keep;
    improv_feed(&im, pkt, wifi_rpc(pkt, "Other", "x"));
    CHECK(packets(p, 16) == 4 && p[2].data[0] == IMPROV_ERROR_UNABLE_TO_CONNECT &&
          p[3].type == IMPROV_TYPE_CURRENT_STATE && p[3].data[0] == IMPROV_STATE_READY);
    improv_feed(&im, pkt, rpc(pkt, IMPROV_CMD_GET_CURRENT_STATE, NULL, 0));
    CHECK(packets(p, 16) == 2 && p[0].data[0] == IMPROV_STATE_PROVISIONED);

    // strings too long for a packet are cut, the packet stays valid
    char big[300];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    const char *two[] = {big, "b"};
    n = improv_result(pkt, 1, two, 2);
    CHECK(n == IMPROV_PACKET_MAX && pkt[8] == 255 && pkt[10] == 253 && pkt[11] == 252);
}

// ---- serve: an emulated board for the SDK test

static pthread_mutex_t s_tx = PTHREAD_MUTEX_INITIALIZER;
static volatile bool s_done;

static void tx(const uint8_t *data, size_t len, void *ctx)
{
    pthread_mutex_lock(&s_tx);  // as the UART driver: a packet is never cut by a log line
    fwrite(data, 1, len, stdout);
    fflush(stdout);
    pthread_mutex_unlock(&s_tx);
}

// log lines like the firmware's, between the packets
static void *logger(void *arg)
{
    for (int i = 0; !s_done; i++) {
        char line[64];
        int n = snprintf(line, sizeof(line), "I (%d) main: tick %d, IMPROV in a log line\n", 1000 + i * 20, i);
        tx((const uint8_t *)line, (size_t)n, NULL);
        usleep(20000);
    }
    return NULL;
}

static bool slow_connect(const char *ssid, const char *password, void *ctx)
{
    usleep(300000);     // the real one waits for the access point
    const char *msg = "I (5000) wifi_sta: connecting\n";
    tx((const uint8_t *)msg, strlen(msg), NULL);
    return fake_connect(ssid, password, ctx);
}

static void serve(void)
{
    improv_t im;
    improv_config_t cfg = {
        .firmware = "ESP32 USB to AirPrint",
        .version = "0.2.0-test",
        .chip = "ESP32-S3",
        .name = "Canon MG2500 (ESP32)",
        .write = tx,
        .url = fake_url,
        .connect = slow_connect,
        .scan = fake_scan,
    };
    improv_init(&im, &cfg);
    pthread_t t;
    pthread_create(&t, NULL, logger, NULL);
    uint8_t buf[256];
    ssize_t n;
    while ((n = read(0, buf, sizeof(buf))) > 0)
        improv_feed(&im, buf, (size_t)n);
    s_done = true;
    pthread_join(t, NULL);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "selftest")) {
        selftest();
        printf("improv: %s\n", failures ? "FAILED" : "all tests passed");
        return failures != 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "serve")) {
        serve();
        return 0;
    }
    fprintf(stderr, "usage: improv_host selftest | serve\n");
    return 2;
}
