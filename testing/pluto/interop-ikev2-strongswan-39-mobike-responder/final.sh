ip xfrm state
ip xfrm pol
if [ -f /var/run/pluto/pluto.pid ]; then ipsec whack --trafficstatus ; fi
if [ -f /var/run/charon.pid ]; then strongswan status ; fi
sleep 7
: ==== cut ====
if [ -f /var/run/pluto/pluto.pid ]; then ipsec auto --status ; fi
if [ -f /var/run/charon.pid ]; then strongswan statusall ; fi
: ==== tuc ====
../bin/check-for-core.sh
if [ -f /sbin/ausearch ]; then ausearch -r -m avc -ts recent ; fi
: ==== end ====
