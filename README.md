C midterm demo starter

What it covers for the March 23 midterm demo:

- multithreaded tracker server
- multithreaded peer server
- manual commands:
  - createtracker
  - updatetracker
  - REQ LIST
  - GET filename.track
- actual peer-to-peer file transfer of the first 1024 bytes

Build:
make

Run tracker:
./tracker config/sconfig

Run peer servers in separate terminals:
./peer Peer1 config/peer1_client.cfg config/peer1_server.cfg serve
./peer Peer2 config/peer2_client.cfg config/peer2_server.cfg serve
./peer Peer3 config/peer3_client.cfg config/peer3_server.cfg serve

Seed a file from Peer1 and create tracker entry:
./peer Peer1 config/peer1_client.cfg config/peer1_server.cfg createtracker sample1.txt sample_file

List files from tracker:
./peer Peer2 config/peer2_client.cfg config/peer2_server.cfg list

Get tracker file:
./peer Peer2 config/peer2_client.cfg config/peer2_server.cfg gettrack sample1.txt.track

Download actual bytes directly from Peer1:
./peer Peer2 config/peer2_client.cfg config/peer2_server.cfg download 127.0.0.1 5001 sample1.txt

Update tracker after partial download:
./peer Peer2 config/peer2_client.cfg config/peer2_server.cfg updatetracker sample1.txt 0 1023

Notes:

- This is intentionally minimal and student-level.
- MD5 is stubbed as dummy_md5 for demo simplicity.
- Download command fetches only the first 1024 bytes, which is enough to prove peer-to-peer transfer.
- GET from the tracker returns the .track file content.
