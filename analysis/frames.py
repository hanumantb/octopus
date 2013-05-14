import sys
from collections import defaultdict

TAG = '[P]'

client_log = open(sys.argv[1])
server_log = open(sys.argv[2])

frame_mapping = {}
frame_starts = {}
frame_ends = {}

# Map sequence numbers to frames.
for line in server_log:
	parts = line.strip().split(' ')[2:]
	if len(parts) == 0 or parts[0] != TAG:
		continue
	seqNum, frameNum = int(parts[2]), int(parts[4])
	frame_mapping[seqNum] = frameNum
	if frameNum not in frame_starts:
		frame_starts[frameNum] = seqNum
	frame_ends[frameNum] = seqNum

frame_counts = defaultdict(int)

def frame_size(frameNum):
	return frame_ends[frameNum] - frame_starts[frameNum] + 1;

start_time = 0

# Calculate client framerate
for line in client_log:
	parts = line.strip().split(' ')
	if parts[0] != TAG:
		continue
	seqNum, time = int(parts[2]), int(parts[4])
	# Skip anything the server has not logged.
	if seqNum not in frame_mapping:
		assert(seqNum == 4294967295)
		continue
		
	if start_time == 0:
		start_time = time
	time -= start_time
	frameNum = frame_mapping[seqNum]
	frame_counts[frameNum] += 1
	frameProgress = float(frame_counts[frameNum]) / frame_size(frameNum)
	print time, frameNum, frameProgress

client_log.close()
server_log.close()