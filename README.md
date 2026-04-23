Build:
make

Run tracker:
./tracker config/sconfig

Run peers:
./peer Peer1 config/peer1_client.cfg config/peer1_server.cfg serve
./peer Peer2 config/peer2_client.cfg config/peer2_server.cfg serve

Manual commands:
./peer Peer1 config/peer1_client.cfg config/peer1_server.cfg createtracker sample1.txt sample_file
./peer Peer2 config/peer2_client.cfg config/peer2_server.cfg list
./peer Peer2 config/peer2_client.cfg config/peer2_server.cfg gettrack sample1.txt.track
./peer Peer2 config/peer2_client.cfg config/peer2_server.cfg updatetracker sample1.txt 0 1023

Full scripted flow:
./final_demo.sh
