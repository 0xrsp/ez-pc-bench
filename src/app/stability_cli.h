#pragma once
// CLI stability mode (--stress ...), see stability_cli.cpp. Called by main_cli when --stress is on the command line.

int stability_cli(int argc, wchar_t** argv);    // after sysinfo_init
void stability_cli_usage();
