#ifndef NETWORK_H
#define NETWORK_H

void connect_wifi(const char *ssid, const char *pass);
void http_task(void *arg);

#endif