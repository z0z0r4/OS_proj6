# Network tests: the rewritten iperf and netperf.
#   part 1: loopback (127.0.0.1), server and client both inside the guest
#   part 2: through virtio-net and qemu's user-mode network to the host
#           peers started by run.sh (10.0.2.2, ports 5311 / 12875)
echo NET_BEGIN

iperf -s -1 &
iperf -c 127.0.0.1 -t 2
iperf -s -u -1 &
iperf -c 127.0.0.1 -u -t 2 -b 5
netserver &
netperf -t TCP_STREAM -l 2
netperf -t UDP_STREAM -l 2
netperf -t TCP_RR -l 2
netperf -t UDP_RR -l 2
netperf -t TCP_CRR -l 2
echo NET_LOOPBACK_DONE

iperf -c 10.0.2.2 -p 5311 -t 2
netperf -H 10.0.2.2 -p 12875 -t TCP_STREAM -l 2
netperf -H 10.0.2.2 -p 12875 -t UDP_STREAM -l 2
netperf -H 10.0.2.2 -p 12875 -t TCP_RR -l 2
netperf -H 10.0.2.2 -p 12875 -t UDP_RR -l 2
netperf -H 10.0.2.2 -p 12875 -t TCP_CRR -l 2
echo NET_DONE
