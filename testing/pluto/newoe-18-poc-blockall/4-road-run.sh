# trigger OE; should show nothing?
../../guestbin/ping-once.sh --forget -I 192.1.3.209 192.1.2.23
ipsec whack --trafficstatus
ipsec whack --shuntstatus
# wait on OE retransmits and rekeying to fail
../../guestbin/wait-for.sh --match oe-failing -- ipsec whack --shuntstatus
ipsec whack --trafficstatus
ipsec _kernel state
ipsec _kernel policy
# ping should fail due to remote block rule
../../guestbin/ping-once.sh --down -I 192.1.3.209 192.1.2.23
# cleanup
killall ip > /dev/null 2> /dev/null
cp /tmp/xfrm-monitor.out OUTPUT/road.xfrm-monitor.txt
echo done
