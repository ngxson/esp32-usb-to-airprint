// The WiFi setup portal on the host: its form decoding, DNS answers and page.
//   wifisetup_host selftest     checks
//   wifisetup_host render DIR   every state of the page -> DIR/wifi-setup-<state>.html
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wifi_setup.h"

static int failures;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                \
        }                                                              \
    } while (0)

static int form(const char *body, const char *key, char *out, size_t n)
{
    return wifi_setup_form_value(body, strlen(body), key, out, n);
}

static void test_form(void)
{
    char v[33];
    CHECK(form("ssid=Home&password=secret12", "ssid", v, sizeof(v)) == 4 && !strcmp(v, "Home"));
    CHECK(form("ssid=Home&password=secret12", "password", v, sizeof(v)) == 8 && !strcmp(v, "secret12"));
    // + is a space, %XX a byte (UTF-8 names), a broken escape stays as it is
    CHECK(form("ssid=My+Net%20%C3%A9%26%3d", "ssid", v, sizeof(v)) > 0 && !strcmp(v, "My Net \xc3\xa9&="));
    CHECK(form("ssid=a%2", "ssid", v, sizeof(v)) == 3 && !strcmp(v, "a%2"));
    CHECK(form("ssid=a%zz", "ssid", v, sizeof(v)) == 4 && !strcmp(v, "a%zz"));
    // keys match whole: "ssid" is not "ssid2" nor "xssid"
    CHECK(form("ssid2=x&xssid=y", "ssid", v, sizeof(v)) == -1 && v[0] == 0);
    CHECK(form("ssid2=x&ssid=y", "ssid", v, sizeof(v)) == 1 && !strcmp(v, "y"));
    // empty values
    CHECK(form("other=&ssid=z", "other", v, sizeof(v)) == 0 && v[0] == 0);
    CHECK(form("other&ssid=z", "other", v, sizeof(v)) == 0);
    CHECK(form("", "ssid", v, sizeof(v)) == -1);
    // 32 bytes fit in 33, 33 do not
    CHECK(form("ssid=12345678901234567890123456789012", "ssid", v, sizeof(v)) == 32);
    CHECK(form("ssid=123456789012345678901234567890123", "ssid", v, sizeof(v)) == -2 && v[0] == 0);
    // the body is not a C string: only len bytes count
    CHECK(wifi_setup_form_value("ssid=abcdef", 8, "ssid", v, sizeof(v)) == 3 && !strcmp(v, "abc"));

    CHECK(wifi_setup_password_ok(""));
    CHECK(!wifi_setup_password_ok("1234567"));
    CHECK(wifi_setup_password_ok("12345678"));
    CHECK(wifi_setup_password_ok("123456789012345678901234567890123456789012345678901234567890123"));
    CHECK(wifi_setup_password_ok("0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789abcdef"));
    CHECK(!wifi_setup_password_ok("0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789abcdeg"));
}

// a query for name (dotted) with the given type, optionally with an EDNS record
static size_t query(uint8_t *q, const char *name, int type, bool edns)
{
    static const uint8_t hdr[12] = {0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0};
    memcpy(q, hdr, 12);
    size_t i = 12;
    while (*name) {
        const char *dot = strchr(name, '.');
        size_t l = dot ? (size_t)(dot - name) : strlen(name);
        q[i++] = (uint8_t)l;
        memcpy(q + i, name, l);
        i += l;
        name += l + (dot ? 1 : 0);
    }
    q[i++] = 0;
    q[i++] = type >> 8;
    q[i++] = type & 0xff;
    q[i++] = 0;
    q[i++] = 1;
    if (edns) {
        q[11] = 1;
        static const uint8_t opt[11] = {0, 0, 41, 0x10, 0, 0, 0, 0, 0, 0, 0};
        memcpy(q + i, opt, sizeof(opt));
        i += sizeof(opt);
    }
    return i;
}

static void test_dns(void)
{
    const uint8_t ip[4] = {192, 168, 4, 1};
    uint8_t q[512], r[512];
    size_t n = query(q, "captive.apple.com", 1, true);
    size_t qlen = n - 11;                  // without the EDNS record
    size_t rn = wifi_setup_dns_reply(q, n, ip, r, sizeof(r));
    CHECK(rn == qlen + 16);
    CHECK(r[0] == 0x12 && r[1] == 0x34);                // id
    CHECK(r[2] == 0x85 && r[3] == 0x80);                // response, authoritative, RD, RA, no error
    CHECK(r[4] == 0 && r[5] == 1 && r[6] == 0 && r[7] == 1 && r[9] == 0 && r[11] == 0);
    CHECK(!memcmp(r + 12, q + 12, qlen - 12));          // the question
    const uint8_t *a = r + qlen;
    CHECK(a[0] == 0xc0 && a[1] == 12 && a[3] == 1 && a[5] == 1 && a[11] == 4 && !memcmp(a + 12, ip, 4));

    // AAAA: no records, still an answer (no error), so the client does not wait
    n = query(q, "connectivitycheck.gstatic.com", 28, false);
    rn = wifi_setup_dns_reply(q, n, ip, r, sizeof(r));
    CHECK(rn == n && r[3] == 0x80 && r[7] == 0);

    // ignored: responses, other opcodes, several questions, truncated or bad names, no room
    n = query(q, "example.com", 1, false);
    q[2] |= 0x80;
    CHECK(wifi_setup_dns_reply(q, n, ip, r, sizeof(r)) == 0);
    q[2] = 0x28;                            // opcode 5 (update)
    CHECK(wifi_setup_dns_reply(q, n, ip, r, sizeof(r)) == 0);
    q[2] = 0x01;
    q[5] = 2;
    CHECK(wifi_setup_dns_reply(q, n, ip, r, sizeof(r)) == 0);
    q[5] = 1;
    for (size_t k = 0; k < n; k++)
        CHECK(wifi_setup_dns_reply(q, k, ip, r, sizeof(r)) == 0);
    CHECK(wifi_setup_dns_reply(q, n, ip, r, n + 15) == 0);
    CHECK(wifi_setup_dns_reply(q, n, ip, r, n + 16) == n + 16);
    q[12] = 0xc0;                           // a pointer in the question
    CHECK(wifi_setup_dns_reply(q, n, ip, r, sizeof(r)) == 0);
    q[12] = 60;                             // a label past the end
    CHECK(wifi_setup_dns_reply(q, n, ip, r, sizeof(r)) == 0);
}

static const wifi_setup_network_t NETS[] = {
    {"Home", -48, true},
    {"Caf\xc3\xa9 <Free> & \"WiFi\"", -70, false},
    {"Neighbour's", -88, true},
};

static wifi_setup_page_t page(wifi_setup_state_t state)
{
    wifi_setup_page_t p = {
        .printer_name = "Canon MG2500 (ESP32)",
        .hostname = "canon-mg2500",
        .networks = NETS,
        .n_networks = 3,
        .state = state,
    };
    if (state != WIFI_SETUP_IDLE)
        p.ssid = "Home";
    if (state == WIFI_SETUP_FAILED)
        p.error = "wrong password";
    if (state == WIFI_SETUP_CONNECTED)
        p.ip = "192.168.2.173";
    return p;
}

static void test_page(void)
{
    static char buf[16384];
    wifi_setup_page_t p = page(WIFI_SETUP_IDLE);
    size_t n = wifi_setup_page(&p, buf, sizeof(buf));
    CHECK(n == strlen(buf) && n < 8192);
    CHECK(strstr(buf, "<form method=post action=/connect>"));
    CHECK(strstr(buf, "value=\"Home\" checked>"));      // the first network is chosen
    // names are escaped in text and attributes
    CHECK(strstr(buf, "value=\"Caf\xc3\xa9 &lt;Free&gt; &amp; &quot;WiFi&quot;\">"));
    CHECK(strstr(buf, "Neighbour&#39;s"));
    CHECK(!strstr(buf, "<Free>"));
    CHECK(strstr(buf, "http://canon-mg2500.local/"));
    CHECK(!strstr(buf, "http-equiv=refresh"));

    p = page(WIFI_SETUP_CONNECTING);
    wifi_setup_page(&p, buf, sizeof(buf));
    CHECK(strstr(buf, "http-equiv=refresh") && strstr(buf, "Connecting to <b>Home</b>") && !strstr(buf, "<form"));

    p = page(WIFI_SETUP_FAILED);
    p.ssid = "Neighbour's";
    wifi_setup_page(&p, buf, sizeof(buf));
    CHECK(strstr(buf, "Could not connect to <b>Neighbour&#39;s</b>: wrong password."));
    CHECK(strstr(buf, "value=\"Neighbour&#39;s\" checked>") && !strstr(buf, "value=\"Home\" checked>"));

    p.ssid = "";
    p.error = "choose a network";
    wifi_setup_page(&p, buf, sizeof(buf));
    CHECK(strstr(buf, "Could not connect: choose a network."));

    p = page(WIFI_SETUP_CONNECTED);
    wifi_setup_page(&p, buf, sizeof(buf));
    CHECK(strstr(buf, "Connected to <b>Home</b>") && strstr(buf, "http://192.168.2.173/") && !strstr(buf, "<form"));

    p = page(WIFI_SETUP_IDLE);
    p.n_networks = 0;
    wifi_setup_page(&p, buf, sizeof(buf));
    CHECK(strstr(buf, "No network found") && strstr(buf, "name=other"));

    // a short buffer: cut, still a string
    p = page(WIFI_SETUP_IDLE);
    CHECK(wifi_setup_page(&p, buf, 100) == 99 && strlen(buf) == 99);
    CHECK(wifi_setup_page(&p, buf, 1) == 0 && buf[0] == 0);
}

static void render(const char *dir)
{
    static const char *const names[] = {"idle", "connecting", "failed", "connected"};
    static char buf[16384];
    for (int s = 0; s < 4; s++) {
        wifi_setup_page_t p = page((wifi_setup_state_t)s);
        size_t n = wifi_setup_page(&p, buf, sizeof(buf));
        char path[512];
        snprintf(path, sizeof(path), "%s/wifi-setup-%s.html", dir, names[s]);
        FILE *f = fopen(path, "wb");
        if (!f) {
            perror(path);
            exit(1);
        }
        fwrite(buf, 1, n, f);
        fclose(f);
    }
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "selftest")) {
        test_form();
        test_dns();
        test_page();
        printf("wifi setup: %s\n", failures ? "FAILED" : "all tests passed");
        return failures != 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "render")) {
        render(argv[2]);
        return 0;
    }
    fprintf(stderr, "usage: wifisetup_host selftest | render DIR\n");
    return 2;
}
