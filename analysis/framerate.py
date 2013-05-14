import sys

frames = {}

end_time = 0

for line in sys.stdin:
	parts = line.strip().split(' ')
	time, frame = int(parts[0]), int(parts[1])
	frames[frame] = 1
	end_time = time

time_seconds = (float(end_time) / 1000)

print "   full framerate (fps): %0.3f" % (len(frames) / time_seconds)
print "partial framerate (fps): %0.3f" % (sum(frames.values()) / time_seconds)