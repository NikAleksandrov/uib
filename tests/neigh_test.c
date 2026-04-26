#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>

#include "uib/neigh.h"

static int g_fail;

static void check(const char *tag, int ok)
{
	printf("%-24s %s\n", tag, ok ? "OK" : "FAIL");
	if (!ok)
		g_fail = 1;
}

static int oracle_entry(char *ip_out, size_t ipcap, uint8_t mac[6])
{
	char line[512];
	FILE *fp = popen("ip -4 neigh show 2>/dev/null", "r");
	int found = 0;

	if (!fp)
		return 0;
	while (!found && fgets(line, sizeof(line), fp)) {
		char ip[128], word[64], mac_s[64];
		unsigned int v[6];
		char *ll = strstr(line, "lladdr ");

		if (!ll)
			continue;
		if (sscanf(line, "%127s", ip) != 1)
			continue;
		if (strchr(ip, ':'))
			continue;
		if (sscanf(ll, "%63s %63s", word, mac_s) != 2)
			continue;
		if (sscanf(mac_s, "%x:%x:%x:%x:%x:%x",
			   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
			continue;
		snprintf(ip_out, ipcap, "%s", ip);
		for (int i = 0; i < 6; i++)
			mac[i] = (uint8_t)v[i];
		found = 1;
	}
	pclose(fp);
	return found;
}

int main(void)
{
	char ip[128];
	uint8_t want[6], got[6];
	uint32_t ip_net, bogus;

	if (oracle_entry(ip, sizeof(ip), want)) {
		inet_pton(AF_INET, ip, &ip_net);
		int rc = uib_neigh_lookup(ip_net, got);

		printf("oracle %s -> %02x:%02x:%02x:%02x:%02x:%02x\n", ip,
		       want[0], want[1], want[2], want[3], want[4], want[5]);
		check("lookup.rc", rc == 0);
		check("lookup.mac_matches", memcmp(want, got, 6) == 0);
	} else {
		printf("lookup.positive          SKIP (no IPv4 neighbor on host)\n");
	}

	inet_pton(AF_INET, "203.0.113.254", &bogus);
	check("lookup.absent", uib_neigh_lookup(bogus, got) != 0);

	printf("RESULT %s\n", g_fail ? "FAIL" : "PASS");
	return g_fail ? 1 : 0;
}
