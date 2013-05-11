package com.glavsoft.transport;

import java.io.DataInput;
import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;

public class UDPDataInput implements DataInput {

	private static final int MTU_SIZE = 1400;
	private final DatagramSocket socket;
	private final byte[] buffer;
	private int index = 0;
	private int size = 0;

	public UDPDataInput(DatagramSocket socket) {
		this.socket = socket;
		this.buffer = new byte[MTU_SIZE * 2];
		this.index = 0;
		this.size = 0;
	}

	private void receive() throws IOException {
		DatagramPacket receivePacket = new DatagramPacket(buffer, buffer.length);
		socket.receive(receivePacket);
		index = 0;
		size = receivePacket.getLength();
	}

	@Override
	public byte readByte() throws IOException {
		// 1 byte.
		if (index > size - 1) {
			receive();
		}
		byte value = buffer[index];
		index++;
		return value;
	}

	@Override
	public short readShort() throws IOException {
		// 2 bytes.
		if (index > size - 2) {
			receive();
		}
		short value = (short) ((buffer[index] << 8) + buffer[index + 1]);
		index += 2;
		return value;
	}

	@Override
	public int readInt() throws IOException {
		// 4 bytes.
		if (index > size - 4) {
			receive();
		}
		int value = ((buffer[index] << 24) + (buffer[index + 1] << 16)
				+ (buffer[index + 2] << 8) + (buffer[index + 3]));
		index += 4;
		return value;
	}

	@Override
	public long readLong() throws IOException {
		// 4 bytes.
		if (index > size - 8) {
			receive();
		}
		long value = ((buffer[index] << 24) + (buffer[index + 1] << 16)
				+ (buffer[index + 2] << 8) + (buffer[index + 3]));
		index += 8;
		return value;
	}

	@Override
	public void readFully(byte[] b, int off, int len) throws IOException {
		// TODO Auto-generated method stub

	}

	@Override
	public int skipBytes(int n) throws IOException {
		// TODO Auto-generated method stub
		return 0;
	}

	@Override
	public boolean readBoolean() throws IOException {
		throw new RuntimeException("NOT SUPPORTED");
	}

	@Override
	public char readChar() throws IOException {
		throw new RuntimeException("NOT SUPPORTED");
	}

	@Override
	public double readDouble() throws IOException {
		throw new RuntimeException("NOT SUPPORTED");
	}

	@Override
	public float readFloat() throws IOException {
		throw new RuntimeException("NOT SUPPORTED");
	}

	@Override
	public void readFully(byte[] b) throws IOException {
		throw new RuntimeException("NOT SUPPORTED");
	}

	@Override
	public String readLine() throws IOException {
		throw new RuntimeException("NOT SUPPORTED");
	}

	@Override
	public String readUTF() throws IOException {
		throw new RuntimeException("NOT SUPPORTED");
	}

	@Override
	public int readUnsignedByte() throws IOException {
		throw new RuntimeException("NOT SUPPORTED");
	}

	@Override
	public int readUnsignedShort() throws IOException {
		throw new RuntimeException("NOT SUPPORTED");
	}

}