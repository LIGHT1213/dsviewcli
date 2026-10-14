/*
 * dslab — analyze/decode/assert command implementations (M3).
 * Part of the dsviewcli project (GPL-3.0-or-later).
 */
#ifndef DSLAB_ANALYZE_CMD_H
#define DSLAB_ANALYZE_CMD_H

/* All take (argc, argv) after the subcommand tokens and return EX_*. */
int cmd_analyze_timing(int argc, char **argv);
int cmd_decode(int argc, char **argv);
int cmd_assert(int argc, char **argv);
int cmd_export(int argc, char **argv);

#endif
