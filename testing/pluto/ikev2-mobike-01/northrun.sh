ipsec auto --up northnet-eastnet
ping -n -q -W 1 -c 2 -I 192.0.3.254  192.0.2.254
ipsec whack --trafficstatus
# note this end should be 192.1.3.33
../../guestbin/ipsec-kernel-state.sh
ip xfrm policy
sleep 5
# remove this end ip next one will take over
ip route show scope global | grep 192.1.3.254 && ip route del default via 192.1.3.254
ip route show scope global | grep 192.1.8.254 || ip route add default via 192.1.8.254
ip addr del 192.1.3.33/24 dev eth1
# let libreswan detect change and do a MOBIKE update
sleep 10
# MOBIKE update and ping should work
# note this end should be 192.1.8.22
ping -n -q -W 1 -c 4 -I 192.0.3.254  192.0.2.254
echo done
