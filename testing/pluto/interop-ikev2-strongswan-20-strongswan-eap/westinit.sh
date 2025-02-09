/testing/guestbin/swan-prep --userland strongswan
# confirm that the network is alive
../../guestbin/wait-until-alive -I 192.0.1.254 192.0.2.254
# ensure that clear text does not get through
iptables -A INPUT -i eth1 -s 192.0.2.0/24 -j DROP
iptables -I INPUT -m policy --dir in --pol ipsec -j ACCEPT
# confirm clear text does not get through
../../guestbin/ping-once.sh --down -I 192.0.1.254 192.0.2.254
cp /testing/x509/certs/east.crt /etc/strongswan/ipsec.d/certs/
cp /testing/x509/real/mainca/root.cert /etc/strongswan/ipsec.d/cacerts/
../../guestbin/strongswan-start.sh
echo "initdone"
