/testing/guestbin/swan-prep --x509
ipsec certutil -A -n 'otherca' -t 'CT,,' -i /testing/x509/cacerts/otherca.crt
# start
ipsec certutil -L

ipsec start
../../guestbin/wait-until-pluto-started
ipsec auto --add nss-cert
ipsec auto --status |grep nss-cert
ipsec whack --impair suppress_retransmits
echo "initdone"
