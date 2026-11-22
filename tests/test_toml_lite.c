/* The TOML subset reader: values, sections, comments, errors. */
#include "psprecomp/toml_lite.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int parses(const char *text) { toml_doc d; const int rc = toml_parse(text, &d); toml_free(&d); return rc == 0; }

int main(void) {
    toml_doc d;
    const char *cfg =
        "# PSP2i client\n"
        "server_ip   = \"192.168.1.20\"   # LAN box\n"
        "server_port = 29001\r\n"
        "lan_discovery = true\n"
        "\n"
        "[player]\n"
        "name = \"Ann \\\"#1\\\"\"\n"
        "level = -1_000\n";
    CHECK(toml_parse(cfg, &d) == 0);
    CHECK(strcmp(toml_get_str(&d, "server_ip", ""), "192.168.1.20") == 0);
    CHECK(toml_get_int(&d, "server_port", 0) == 29001);
    CHECK(toml_get_bool(&d, "lan_discovery", 0) == 1);
    CHECK(strcmp(toml_get_str(&d, "player.name", ""), "Ann \"#1\"") == 0);
    CHECK(toml_get_int(&d, "player.level", 0) == -1000);
    CHECK(toml_get_int(&d, "server_ip", 7) == 7);                 /* wrong type -> default */
    CHECK(strcmp(toml_get_str(&d, "missing", "def"), "def") == 0);
    toml_free(&d);

    CHECK(!parses("a = [1, 2]\n"));
    CHECK(!parses("a = \"unterminated\n"));
    CHECK(!parses("a = 1 2\n"));
    CHECK(!parses("= 1\n"));
    CHECK(!parses("a = \"\"\"x\"\"\"\n"));
    CHECK(toml_parse("ok = 1\nbad line\n", &d) == -1 && strstr(d.error, "line 2"));
    CHECK(parses(""));
    CHECK(toml_parse_file("/nonexistent/client_config.toml", &d) == -2);

    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    puts("toml_lite: ok");
    return 0;
}
