#!/bin/bash
set -e
cd "$(cd "$(dirname "$0")" && pwd)"

pkill -f "./tracker" 2>/dev/null || true
pkill -f "./peer" 2>/dev/null || true
sleep 1
rm -f torrents/*.track
for i in $(seq 1 13); do
  rm -f peer${i}_shared/*.track peer${i}_shared/sample1.txt peer${i}_shared/large_file.bin 2>/dev/null || true
done

./tracker config/sconfig & TRACKER_PID=$!
sleep 1
./peer Peer1 config/peer1_client.cfg config/peer1_server.cfg serve & PEER1_PID=$!
./peer Peer2 config/peer2_client.cfg config/peer2_server.cfg serve & PEER2_PID=$!
sleep 2

./peer Peer1 config/peer1_client.cfg config/peer1_server.cfg createtracker sample1.txt Small_sample_file
./peer Peer2 config/peer2_client.cfg config/peer2_server.cfg createtracker large_file.bin Large_binary_file

sleep 28
for i in 3 4 5 6 7 8; do
  mkdir -p peer${i}_shared
  ./peer Peer${i} config/peer${i}_client.cfg config/peer${i}_server.cfg autodownload &
done

sleep 60
for i in 9 10 11 12 13; do
  mkdir -p peer${i}_shared
  ./peer Peer${i} config/peer${i}_client.cfg config/peer${i}_server.cfg autodownload &
done

kill $PEER1_PID 2>/dev/null && echo "Peer1 terminated" || true
kill $PEER2_PID 2>/dev/null && echo "Peer2 terminated" || true

wait || true
kill $TRACKER_PID 2>/dev/null || true
