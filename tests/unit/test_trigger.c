/* Unit tests for trigger spec parsing (tests/unit, no hardware). */
#include <stdio.h>
#include <string.h>
#include "trigger.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

int test_trigger_run(void)
{
    dslab_trig_spec t;
    char err[160];
    int rc;

    rc = dslab_trig_parse("D0:rising", &t, err, sizeof(err));
    CHECK(rc == 0, "D0:rising parses");
    CHECK(t.count == 1 && t.items[0].ch_index == 0 && t.items[0].trig_char == 'R',
          "D0:rising maps to R");

    rc = dslab_trig_parse("D3:falling", &t, err, sizeof(err));
    CHECK(rc == 0 && t.items[0].ch_index == 3 && t.items[0].trig_char == 'F',
          "D3:falling maps to F");

    rc = dslab_trig_parse("D1:f", &t, err, sizeof(err));
    CHECK(rc == 0 && t.items[0].trig_char == 'F', "short 'f' works");

    rc = dslab_trig_parse("D0:high", &t, err, sizeof(err));
    CHECK(rc == 0 && t.items[0].trig_char == '1', "high -> '1'");
    rc = dslab_trig_parse("D0:LOW", &t, err, sizeof(err));
    CHECK(rc == 0 && t.items[0].trig_char == '0', "LOW (case-insensitive) -> '0'");
    rc = dslab_trig_parse("D2:change", &t, err, sizeof(err));
    CHECK(rc == 0 && t.items[0].trig_char == 'C', "change -> 'C'");

    rc = dslab_trig_parse("D0:rising,D1:low", &t, err, sizeof(err));
    CHECK(rc == 0 && t.count == 2, "multi-channel parses");
    CHECK(t.items[0].ch_index == 0 && t.items[1].ch_index == 1, "order kept");
    CHECK(t.items[0].trig_char == 'R' && t.items[1].trig_char == '0',
          "multi-channel chars");

    rc = dslab_trig_parse("D5:duplicate,D5:rising", &t, err, sizeof(err));
    CHECK(rc == -1, "duplicate channel rejected");

    rc = dslab_trig_parse("X9:rising", &t, err, sizeof(err));
    CHECK(rc == -1, "bad channel name rejected");

    rc = dslab_trig_parse("D9:sideways", &t, err, sizeof(err));
    CHECK(rc == -1, "unknown condition rejected");

    rc = dslab_trig_parse("", &t, err, sizeof(err));
    CHECK(rc == -1, "empty spec rejected");

    rc = dslab_trig_parse("D0", &t, err, sizeof(err));
    CHECK(rc == -1, "missing colon rejected");

    return failures;
}
