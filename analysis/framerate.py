import sys

frames = {}

end_time = 0

for line in sys.stdin:
	parts = line.strip().split(' ')
	time, frame = int(parts[0]), int(parts[1])
	frames[frame] = 1
	end_time = time

print "framerate (fps): %f" % (len(frames) / (float(end_time) / 1000))