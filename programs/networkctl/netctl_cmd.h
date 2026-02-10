// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKCTL_CMD_H
#define YOS_NETWORKCTL_CMD_H

int netctl_cmd_status(int show_links);
int netctl_cmd_links(void);
int netctl_cmd_ping(int argc, char** argv);
int netctl_cmd_resolve(int argc, char** argv);
int netctl_cmd_config(int argc, char** argv);
int netctl_cmd_iface(int up);
int netctl_cmd_daemon(int argc, char** argv);

#endif
