#include <string.h>
#include <stdio.h>
#include <assert.h>

#include <netinet/ether.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <netinet/tcp.h>
#include <netinet/ip_icmp.h>
#include <netinet/igmp.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../exanic.h"
#include "parser.h"

enum
{
    ETH_END = sizeof(struct ethhdr),
    IP_END  = sizeof(struct ethhdr) + sizeof(struct iphdr),
};

int exanic_parse_filter_string(const char *filter, char *pattern, char *mask,
                               int *drop_rule)
{
    char buf[EXANIC_FILTER_STRING_MAX_LEN];
    char *strtok_ptr = NULL, *tok = NULL;
    int ip = 0, tcp = 0, udp = 0, arp = 0, icmp = 0, igmp = 0;
    int consume_proto = 0;
    struct ethhdr *ethhdr_pat = NULL, *ethhdr_mask = NULL;
    struct iphdr *iphdr_pat = NULL, *iphdr_mask = NULL;
    struct tcphdr *tcphdr_pat = NULL, *tcphdr_mask = NULL;
    struct udphdr *udphdr_pat = NULL, *udphdr_mask = NULL;
    struct icmphdr *icmphdr_pat = NULL, *icmphdr_mask = NULL;
    struct ether_arp *arp_pat = NULL, *arp_mask = NULL;

    if (strlen(filter) > EXANIC_FILTER_STRING_MAX_LEN - 1)
    {
        exanic_err_printf("filter too long");
        return -1;
    }

    strcpy(buf, filter);

    memset(pattern, 0, EXANIC_FILTER_SIZE);
    memset(mask, 0, EXANIC_FILTER_SIZE);

    /* First token is 'allow' or 'drop' */
    tok = strtok_r(buf, " ", &strtok_ptr);
    if (tok == NULL)
    {
        exanic_err_printf("parse error: expected 'allow' or 'drop'");
        return -1;
    }
    if (strcmp(tok, "allow") == 0)
        *drop_rule = 0;
    else if (strcmp(tok, "drop") == 0)
        *drop_rule = 1;
    else
    {
        exanic_err_printf("parse error: "
                "expected 'allow' or 'drop', got '%s'", tok);
        return -1;
    }

    tok = strtok_r(NULL, " ", &strtok_ptr);
    if (tok == NULL)
        /* Empty filter, matches everything */
        return 0;

    /* Second token is the protocol (optional) */
    if (strcmp(tok, "tcp") == 0)
        ip = tcp = consume_proto = 1;
    else if (strcmp(tok, "udp") == 0)
        ip = udp = consume_proto = 1;
    else if (strcmp(tok, "icmp") == 0)
        ip = icmp = consume_proto = 1;
    else if (strcmp(tok, "igmp") == 0)
        ip = igmp = consume_proto = 1;
    else if (strcmp(tok, "arp") == 0)
        arp = consume_proto = 1;
    else if (strcmp(tok, "ip") == 0)
        ip = consume_proto = 1;
    else
        ip = 1;

    if (consume_proto)
        tok = strtok_r(NULL, " ", &strtok_ptr);

    ethhdr_pat = (struct ethhdr *)pattern;
    ethhdr_mask = (struct ethhdr *)mask;

    if (ip)
    {
        iphdr_pat = (struct iphdr *)(pattern + ETH_END);
        iphdr_mask = (struct iphdr *)(mask + ETH_END);
        ethhdr_pat->h_proto = ntohs(ETH_P_IP);
        ethhdr_mask->h_proto = 0xFFFF;
        iphdr_pat->ihl = 5;
        iphdr_mask->ihl = 0xF;
        iphdr_pat->version = 4;
        iphdr_mask->version = 0xF;
    }
    else if (arp)
    {
        arp_pat = (struct ether_arp *)(pattern + ETH_END);
        arp_mask = (struct ether_arp *)(mask + ETH_END);
        ethhdr_pat->h_proto = ntohs(ETH_P_ARP);
        ethhdr_mask->h_proto = 0xFFFF;
        arp_pat->arp_hrd = htons(ARPHRD_ETHER);
        arp_mask->arp_hrd = 0xFFFF;
        arp_pat->arp_pro = htons(ETH_P_IP);
        arp_mask->arp_pro = 0xFFFF;
        arp_pat->arp_hln = ETH_ALEN;
        arp_mask->arp_hln = 0xFF;
        arp_pat->arp_pln = sizeof(struct in_addr);
        arp_mask->arp_pln = 0xFF;
    }

    if (tcp)
    {
        tcphdr_pat = (struct tcphdr *)(pattern + IP_END);
        tcphdr_mask = (struct tcphdr *)(mask + IP_END);
        iphdr_pat->protocol = IPPROTO_TCP;
        iphdr_mask->protocol = 0xFF;
    }
    else if (udp)
    {
        udphdr_pat = (struct udphdr *)(pattern + IP_END);
        udphdr_mask = (struct udphdr *)(mask + IP_END);
        iphdr_pat->protocol = IPPROTO_UDP;
        iphdr_mask->protocol = 0xFF;
    }
    else if (icmp)
    {
        icmphdr_pat = (struct icmphdr *)(pattern + IP_END);
        icmphdr_mask = (struct icmphdr *)(mask + IP_END);
        iphdr_pat->protocol = IPPROTO_ICMP;
        iphdr_mask->protocol = 0xFF;
    }
    else if (igmp)
    {
        iphdr_pat->protocol = IPPROTO_IGMP;
        iphdr_mask->protocol = 0xFF;
        iphdr_pat->ihl = 0;
        iphdr_mask->ihl = 0x0;
    }

    /* Loop over remaining tokens */
    while (tok != NULL)
    {
        int dst = 0, src = 0, type = 0;

        /* Expected 'dst', 'src' or 'type' */
        if (strcmp(tok, "dst") == 0)
            dst = 1;
        else if (strcmp(tok, "src") == 0)
            src = 1;
        else if (strcmp(tok, "type") == 0)
            type = 1;
        else
        {
            exanic_err_printf("parse error: "
                    "expected 'dst', 'src' or 'type', got '%s'", tok);
            return -1;
        }

        if (dst || src)
        {
            int port = 0, host = 0, net = 0;
            uint16_t port_num = 0;
            in_addr_t ip_addr = 0;
            in_addr_t ip_netmask = 0;

            /* Expected TCP/UDP port or host or net specification */
            tok = strtok_r(NULL, " ", &strtok_ptr);
            if (tok == NULL)
            {
                exanic_err_printf("parse error");
                return -1;
            }
            if (strcmp(tok, "port") == 0)
                port = 1;
            else if (strcmp(tok, "host") == 0)
                host = 1;
            else if (strcmp(tok, "net") == 0)
                net = 1;
            else
            {
                exanic_err_printf("parse error: "
                        "expected 'port', 'host' or 'net', got '%s'", tok);
                return -1;
            }

            if (port && !(tcp || udp))
            {
                exanic_err_printf("port requires tcp or udp");
                return -1;
            }

            if (dst && arp)
            {
                /* Target IP does not fit in the first 40 bytes */
                exanic_err_printf("filtering on arp target not supported");
                return -1;
            }

            tok = strtok_r(NULL, " ", &strtok_ptr);
            if (tok == NULL)
            {
                exanic_err_printf("parse error");
                return -1;
            }
            if (port)
            {
                char *endptr;
                port_num = strtol(tok, &endptr, 10);
                if (*endptr != '\0')
                {
                    exanic_err_printf("parse error: invalid port '%s'", tok);
                    return -1;
                }
            }
            else if (host)
            {
                struct in_addr in;
                if (inet_aton(tok, &in) == 0)
                {
                    exanic_err_printf("parse error: invalid address '%s'", tok);
                    return -1;
                }
                ip_addr = in.s_addr;
                ip_netmask = 0xFFFFFFFF;
            }
            else if (net)
            {
                char *p, *addr_str, *bits_str;
                int bits;
                struct in_addr in;

                addr_str = strtok_r(tok, "/", &p);
                bits_str = strtok_r(NULL, "", &p);
                if (addr_str == NULL || bits_str == NULL)
                {
                    exanic_err_printf("parse error: invalid network '%s'", tok);
                    return -1;
                }

                if (inet_aton(addr_str, &in) == 0)
                {
                    exanic_err_printf("parse error: "
                            "invalid address '%s'", addr_str);
                    return -1;
                }

                bits = strtol(bits_str, &p, 10);
                if (*p != '\0' || bits < 0 || bits > 32)
                {
                    exanic_err_printf("parse error: "
                            "invalid prefix length '%s'", bits_str);
                    return -1;
                }

                ip_addr = in.s_addr;
                ip_netmask = htonl(0xFFFFFFFF << (32 - bits));
            }

            /* Update pattern and mask */
            if (port)
            {
                if (tcp)
                {
                    if (dst)
                    {
                        if (tcphdr_mask->dest != 0)
                        {
                            exanic_err_printf("destination port conflict");
                            return -1;
                        }
                        tcphdr_pat->dest = htons(port_num);
                        tcphdr_mask->dest = 0xFFFF;
                    }
                    else if (src)
                    {
                        if (tcphdr_mask->source != 0)
                        {
                            exanic_err_printf("source port conflict");
                            return -1;
                        }
                        tcphdr_pat->source = htons(port_num);
                        tcphdr_mask->source = 0xFFFF;
                    }
                }
                else if (udp)
                {
                    if (dst)
                    {
                        if (udphdr_mask->dest != 0)
                        {
                            exanic_err_printf("destination port conflict");
                            return -1;
                        }
                        udphdr_pat->dest = htons(port_num);
                        udphdr_mask->dest = 0xFFFF;
                    }
                    else if (src)
                    {
                        if (udphdr_mask->source != 0)
                        {
                            exanic_err_printf("source port conflict");
                            return -1;
                        }
                        udphdr_pat->source = htons(port_num);
                        udphdr_mask->source = 0xFFFF;
                    }
                }
            }
            else if (host || net)
            {
                if (ip)
                {
                    if (dst)
                    {
                        if (iphdr_mask->daddr != 0)
                        {
                            exanic_err_printf("destination address conflict");
                            return -1;
                        }
                        iphdr_pat->daddr = ip_addr;
                        iphdr_mask->daddr = ip_netmask;
                    }
                    else if (src)
                    {
                        if (iphdr_mask->saddr != 0)
                        {
                            exanic_err_printf("source address conflict");
                            return -1;
                        }
                        iphdr_pat->saddr = ip_addr;
                        iphdr_mask->saddr = ip_netmask;
                    }
                }
                else if (arp)
                {
                    in_addr_t z = 0;
                    assert(!dst);
                    if (memcmp(arp_mask->arp_spa, &z, sizeof(in_addr_t)) != 0)
                    {
                        exanic_err_printf("destination address conflict");
                        return -1;
                    }
                    memcpy(arp_pat->arp_spa, &ip_addr, sizeof(in_addr_t));
                    memcpy(arp_mask->arp_spa, &ip_netmask, sizeof(in_addr_t));
                }
            }
        }
        else if (type)
        {
            char *endptr;
            uint8_t type_code;

            if (!icmp)
            {
                exanic_err_printf("type requires icmp");
                return -1;
            }

            tok = strtok_r(NULL, " ", &strtok_ptr);
            if (tok == NULL)
            {
                exanic_err_printf("parse error");
                return -1;
            }

            type_code = strtol(tok, &endptr, 10);
            if (*endptr != '\0')
            {
                exanic_err_printf("parse error: invalid type '%s'", tok);
                return -1;
            }

            icmphdr_pat->type = type_code;
            icmphdr_mask->type = 0xFF;
        }

        tok = strtok_r(NULL, " ", &strtok_ptr);
    }

    return 0;
}
