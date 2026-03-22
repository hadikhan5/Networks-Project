#!/bin/bash
set -e
echo "Build first: make"
echo "1) Run tracker: ./tracker config/sconfig"
echo "2) Run peer servers:"
echo "   ./peer Peer1 config/peer1_client.cfg config/peer1_server.cfg serve"
echo "   ./peer Peer2 config/peer2_client.cfg config/peer2_server.cfg serve"
echo "3) In a new terminal:"
echo "   ./peer Peer1 config/peer1_client.cfg config/peer1_server.cfg createtracker sample1.txt sample_file"
echo "   ./peer Peer2 config/peer2_client.cfg config/peer2_server.cfg list"
echo "   ./peer Peer2 config/peer2_client.cfg config/peer2_server.cfg gettrack sample1.txt.track"
echo "   ./peer Peer2 config/peer2_client.cfg config/peer2_server.cfg download 127.0.0.1 5001 sample1.txt"
echo "   ./peer Peer2 config/peer2_client.cfg config/peer2_server.cfg updatetracker sample1.txt 0 1023"
