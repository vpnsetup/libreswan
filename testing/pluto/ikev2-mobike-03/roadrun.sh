ipsec auto --up road-eastnet
ping -n -q -W 1 -c 2 192.0.2.254
ipsec whack --trafficstatus
# note this end should be 192.1.3.209
../../guestbin/ipsec-kernel-state.sh
ip xfrm policy
sleep 5
# remove this end ip next one will take over
ip route show scope global | grep 192.1.3.254 && ip route del default via 192.1.3.254
ip route show scope global | grep 192.1.33.254 || ip route add default via 192.1.33.254
ip addr del 192.1.3.209/24 dev eth0
# let libreswan detect change and do a MOBIKE update
sleep 10
# both ends updated MOBIKE ping should work
# note this end should be 192.1.33.222
ping -n -q -W 1 -c 4 192.0.2.254
echo done
