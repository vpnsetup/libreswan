# in the clear; then establish
../../guestbin/ping-once.sh --up 192.1.2.23
ipsec auto --up road-east-x509-ipv4
../../guestbin/ping-once.sh --up -I 192.0.2.100 192.1.2.23
ipsec whack --trafficstatus

# Let R_U_THERE packets flow
sleep 15

# Setting up block via iptables
iptables -I INPUT -s 192.1.2.23/32 -d 0/0 -j DROP
iptables -I OUTPUT -d 192.1.2.23/32 -s 0/0 -j DROP

# Wait for liveness to trigger
../../guestbin/wait-for.sh --match '^".*#1: liveness action' -- cat /tmp/pluto.log

# Tunnel should be cleared, no trap/hold
# shuntstatus does not show connection shunt unfortunately
ipsec whack --trafficstatus
ipsec whack --shuntstatus
