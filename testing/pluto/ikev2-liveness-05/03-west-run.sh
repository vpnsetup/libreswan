# establish the tunnel
ipsec auto --up westnet-eastnet-ikev2
../../guestbin/ping-once.sh --up -I 192.0.1.254 192.0.2.254
ipsec whack --trafficstatus

# Let R_U_THERE packets flow
sleep 15

# Setting up block via iptables
iptables -I INPUT -s 192.1.2.23/32 -d 0/0 -j DROP
iptables -I OUTPUT -d 192.1.2.23/32 -s 0/0 -j DROP

# DPD should have triggered now
../../guestbin/wait-for.sh --match '^".*#1: liveness action' -- cat /tmp/pluto.log

# Tunnel should be down with %trap or %hold preventing packet leaks
# But shuntstatus only shows bare shunts, not connection shunts :(
ipsec whack --trafficstatus
ipsec whack --shuntstatus

# packets should be caught in firewall and no icmp replies should
# happen
../../guestbin/ping-once.sh --down -I 192.0.1.254 192.0.2.254

# Remove the Blockage
iptables -D INPUT -s 192.1.2.23/32 -d 0/0 -j DROP
iptables -D OUTPUT -d 192.1.2.23/32 -s 0/0 -j DROP

# Tunnel should be back up now even without triggering traffic
../../guestbin/wait-for.sh --match '^".*#[3-9] established Child SA' -- cat /tmp/pluto.log
../../guestbin/ping-once.sh --up -I 192.0.1.254 192.0.2.254
