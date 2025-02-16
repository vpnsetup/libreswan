# east initiated sending an IKE_SA_INIT request;
# hold east's IKE_SA_INIT request as inbound message 1
../../guestbin/wait-for.sh --match 'IMPAIR: blocking inbound message 1' -- cat /tmp/pluto.log

# on west, respond to east's IKE_SA_INIT request (message 1)
# create east's IKE SA #1;
# hold east's IKE_AUTH request as inbound message 2
ipsec whack --impair drip-inbound:1
../../guestbin/wait-for.sh --match '^".*#1: sent IKE_SA_INIT' -- cat /tmp/pluto.log
../../guestbin/wait-for.sh --match 'IMPAIR: blocking inbound message 2' -- cat /tmp/pluto.log

# on west, initiate creating west's IKE SA #2;
# hold east's IKE_SA_INIT response as inbound message 3
ipsec up --asynchronous east-west
../../guestbin/wait-for.sh --match '^".*#2: sent IKE_SA_INIT request' -- cat /tmp/pluto.log
../../guestbin/wait-for.sh --match 'IMPAIR: blocking inbound message 3' -- cat /tmp/pluto.log

# on west, process east's IKE_SA_INIT response (message 3);
# establish west's IKE SA #2; and create west's Child SA #4
# hold east's IKE_AUTH response as inbound message 4
ipsec whack --impair drip-inbound:3
../../guestbin/wait-for.sh --match '^".*#2: sent IKE_AUTH request'  -- cat /tmp/pluto.log
../../guestbin/wait-for.sh --match 'IMPAIR: blocking inbound message 4'  -- cat /tmp/pluto.log

# on west, process east's IKE_SA_AUTH response (message 4)
# establish west's Child SA #3
ipsec whack --impair drip-inbound:4
../../guestbin/wait-for.sh --match 'established Child SA using #2' -- cat /tmp/pluto.log

# on west, process east's IKE_SA_AUTH request (message 2)
# establish east's IKE SA #1 and create east's Child SA #3
ipsec whack --impair drip-inbound:2
../../guestbin/wait-for.sh --match 'established Child SA using #1'  -- cat /tmp/pluto.log
