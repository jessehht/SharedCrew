@echo off
rem Test peer that only answers through the SharedCrew server relay (as if a direct connection is impossible).
"%~dp0SharedCrew-TestPeer.exe" --relay-only
pause
