ping -n -q -c 1 -I 192.1.3.209 192.1.2.23
# wait on OE retransmits and rekeying
sleep 5
# no tunnel and no bare shunts expected
ipsec whack --trafficstatus
ipsec whack --shuntstatus
../../guestbin/ipsec-kernel-state.sh
../../guestbin/ipsec-kernel-policy.sh
killall ip > /dev/null 2> /dev/null
cp /tmp/xfrm-monitor.out OUTPUT/road.xfrm-monitor.txt
# ping should succeed due to mutual pass policy
ping -n -q -c 2 -I 192.1.3.209 192.1.2.23
echo done
