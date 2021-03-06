// Copyright (C) 2010, 2011, 2012, 2013 GlavSoft LLC.
// All rights reserved.
//
//-------------------------------------------------------------------------
// This file is part of the TightVNC software.  Please visit our Web site:
//
//                       http://www.tightvnc.com/
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//-------------------------------------------------------------------------
//

package com.glavsoft.rfb.protocol;

import java.io.IOException;
import java.net.DatagramSocket;
import java.net.Socket;
import java.net.SocketException;
import java.util.logging.Logger;

import com.glavsoft.core.SettingsChangedEvent;
import com.glavsoft.drawing.Renderer;
import com.glavsoft.exceptions.AuthenticationFailedException;
import com.glavsoft.exceptions.FatalException;
import com.glavsoft.exceptions.TransportException;
import com.glavsoft.exceptions.UnsupportedProtocolVersionException;
import com.glavsoft.exceptions.UnsupportedSecurityTypeException;
import com.glavsoft.rfb.ClipboardController;
import com.glavsoft.rfb.IChangeSettingsListener;
import com.glavsoft.rfb.IPasswordRetriever;
import com.glavsoft.rfb.IRepaintController;
import com.glavsoft.rfb.IRfbSessionListener;
import com.glavsoft.rfb.client.ClientToServerMessage;
import com.glavsoft.rfb.client.FramebufferUpdateRequestMessage;
import com.glavsoft.rfb.client.SetEncodingsMessage;
import com.glavsoft.rfb.client.SetPixelFormatMessage;
import com.glavsoft.rfb.encoding.PixelFormat;
import com.glavsoft.rfb.encoding.decoder.DecodersContainer;
import com.glavsoft.rfb.protocol.state.HandshakeState;
import com.glavsoft.rfb.protocol.state.ProtocolState;
import com.glavsoft.transport.Reader;
import com.glavsoft.transport.UDPInputStream;
import com.glavsoft.transport.Writer;
import com.glavsoft.viewer.RfbConnectionWorker;

public class Protocol implements ProtocolContext, IChangeSettingsListener {
	private static final int RECONNECT_WAIT_TIME = 2000;
	private ProtocolState state;
	private final Logger logger = Logger.getLogger("com.glavsoft.rfb.protocol");
	private final IPasswordRetriever passwordRetriever;
	private final ProtocolSettings settings;
	private int fbWidth;
	private int fbHeight;
	private PixelFormat pixelFormat;
	private Reader reader;
	private Writer writer;
	private String remoteDesktopName;
	private MessageQueue messageQueue;
	private final DecodersContainer decoders;
	private SenderTask senderTask;
	private ReceiverTask receiverTask;
	private IRfbSessionListener rfbSessionListener;
	private IRepaintController repaintController;
	private ClipboardController clipboardController;
	private PixelFormat serverPixelFormat;
	private Thread senderThread;
	private Thread receiverThread;
    private boolean isTight;
    private String protocolVersion;
	private Socket workingSocket;
	private final RfbConnectionWorker worker;

    public Protocol(Reader reader, Writer writer,
			IPasswordRetriever passwordRetriever, ProtocolSettings settings,
			Socket workingSocket, RfbConnectionWorker worker) {
		this.reader = reader;
		this.writer = writer;
		this.passwordRetriever = passwordRetriever;
		this.settings = settings;
		this.workingSocket = workingSocket;
		this.worker = worker;
		decoders = new DecodersContainer();
		decoders.instantiateDecodersWhenNeeded(settings.encodings);
		state = new HandshakeState(this);
	}

	@Override
	public void changeStateTo(ProtocolState state) {
		this.state = state;
	}

	public void handshake() throws UnsupportedProtocolVersionException, UnsupportedSecurityTypeException,
			AuthenticationFailedException, TransportException, FatalException {
		while (state.next()) {
			// continue;
		}
		this.messageQueue = new MessageQueue();
	}

	@Override
	public PixelFormat getPixelFormat() {
		return pixelFormat;
	}

	@Override
	public void setPixelFormat(PixelFormat pixelFormat) {
		this.pixelFormat = pixelFormat;
		if (repaintController != null) {
			repaintController.setPixelFormat(pixelFormat);
		}
	}

	@Override
	public String getRemoteDesktopName() {
		return remoteDesktopName;
	}

	@Override
	public void setRemoteDesktopName(String name) {
		remoteDesktopName = name;
	}

	@Override
	public int getFbWidth() {
		return fbWidth;
	}

	@Override
	public void setFbWidth(int fbWidth) {
		this.fbWidth = fbWidth;
	}

	@Override
	public int getFbHeight() {
		return fbHeight;
	}

	@Override
	public void setFbHeight(int fbHeight) {
		this.fbHeight = fbHeight;
	}

	@Override
	public IPasswordRetriever getPasswordRetriever() {
		return passwordRetriever;
	}

	@Override
	public ProtocolSettings getSettings() {
		return settings;
	}

	@Override
	public Logger getLogger() {
		return logger;
	}

	@Override
	public Writer getWriter() {
		return writer;
	}

	@Override
	public Reader getReader() {
		return reader;
	}

	/**
	 * Following the server initialisation message it's up to the client to send
	 * whichever protocol messages it wants.  Typically it will send a
	 * SetPixelFormat message and a SetEncodings message, followed by a
	 * FramebufferUpdateRequest.  From then on the server will send
	 * FramebufferUpdate messages in response to the client's
	 * FramebufferUpdateRequest messages.  The client should send
	 * FramebufferUpdateRequest messages with incremental set to true when it has
	 * finished processing one FramebufferUpdate and is ready to process another.
	 * With a fast client, the rate at which FramebufferUpdateRequests are sent
	 * should be regulated to avoid hogging the network.
	 */
	public void startNormalHandling(IRfbSessionListener rfbSessionListener,
			IRepaintController repaintController, ClipboardController clipboardController) {
		this.rfbSessionListener = rfbSessionListener;
		this.repaintController = repaintController;
		this.clipboardController = clipboardController;
//		if (settings.getBitsPerPixel() == 0) {
//			settings.setBitsPerPixel(pixelFormat.bitsPerPixel); // the same the server sent when not initialized yet
//		}
		serverPixelFormat = pixelFormat;
		serverPixelFormat.trueColourFlag = 1; // correct flag - we don't support color maps
		setPixelFormat(createPixelFormat(settings));
		sendMessage(new SetPixelFormatMessage(pixelFormat));
		logger.fine("sent: "+pixelFormat);

		sendSupportedEncodingsMessage(settings);
		settings.addListener(this); // to support pixel format (color depth), and encodings changes
		settings.addListener(repaintController);

		sendRefreshMessage();

		Renderer renderer = repaintController.createRenderer(reader, getFbWidth(), getFbHeight(), getPixelFormat());

		UDPInputStream udpInputStream;
		try {
			udpInputStream = new UDPInputStream(new DatagramSocket(6829));
			ReceiverTask udpReceiverTask = new ReceiverTask(
					new Reader(udpInputStream), repaintController,
					clipboardController,
					decoders, this, renderer);
			Thread udpThread = new Thread(udpReceiverTask, "UdpReceiverTask");
			udpThread.start();
		} catch (SocketException e) {
			e.printStackTrace();
		}
		
		senderTask = new SenderTask(messageQueue, writer, this);
		senderThread = new Thread(senderTask, "RfbSenderTask");
		senderThread.start();
		decoders.resetDecoders();
		receiverTask = new ReceiverTask(
				reader, repaintController,
				clipboardController,
				decoders, this, renderer);
		receiverThread = new Thread(receiverTask, "RfbReceiverTask");
		receiverThread.start();
	}

	@Override
	public void sendMessage(ClientToServerMessage message) {
		messageQueue.put(message);
	}

	private void sendSupportedEncodingsMessage(ProtocolSettings settings) {
		decoders.instantiateDecodersWhenNeeded(settings.encodings);
		SetEncodingsMessage encodingsMessage = new SetEncodingsMessage(settings.encodings);
		sendMessage(encodingsMessage);
		logger.fine("sent: " + encodingsMessage.toString());
	}

	/**
	 * create pixel format by bpp
	 */
	private PixelFormat createPixelFormat(ProtocolSettings settings) {
		int serverBigEndianFlag = serverPixelFormat.bigEndianFlag;
		switch (settings.getBitsPerPixel()) {
		case ProtocolSettings.BPP_32:
			return PixelFormat.create32bppPixelFormat(serverBigEndianFlag);
		case ProtocolSettings.BPP_16:
			return PixelFormat.create16bppPixelFormat(serverBigEndianFlag);
		case ProtocolSettings.BPP_8:
			return PixelFormat.create8bppBGRPixelFormat(serverBigEndianFlag);
		case ProtocolSettings.BPP_6:
			return PixelFormat.create6bppPixelFormat(serverBigEndianFlag);
		case ProtocolSettings.BPP_3:
			return PixelFormat.create3bppPixelFormat(serverBigEndianFlag);
		case ProtocolSettings.BPP_SERVER_SETTINGS:
			return serverPixelFormat;
		default:
			// unsupported bpp, use default
			return PixelFormat.create32bppPixelFormat(serverBigEndianFlag);
		}
	}

	@Override
	public void settingsChanged(SettingsChangedEvent e) {
		ProtocolSettings settings = (ProtocolSettings) e.getSource();
		if (settings.isChangedEncodings()) {
			sendSupportedEncodingsMessage(settings);
		}
		if (settings.changedBitsPerPixel() && receiverTask != null) {
			receiverTask.queueUpdatePixelFormat(createPixelFormat(settings));
		}
	}

	@Override
	public void sendRefreshMessage() {
		sendMessage(new FramebufferUpdateRequestMessage(0, 0, fbWidth, fbHeight, false));
		logger.fine("sent: full FB Refresh");
	}

	@Override
	public void cleanUpSession(String message) {
		cleanUpSession();
		rfbSessionListener.rfbSessionStopped(message);
	}

	public synchronized void cleanUpSession() {
		if (senderTask != null) { senderTask.stopTask(); }
		if (receiverTask != null) { receiverTask.stopTask(); }
		if (senderTask != null) {
			try {
				senderThread.join(1000);
			} catch (InterruptedException e) {
				// nop
			}
			senderTask = null;
		}
		if (receiverTask != null) {
			try {
				receiverThread.join(1000);
			} catch (InterruptedException e) {
				// nop
			}
			receiverTask = null;
		}
	}

    @Override
    public void setTight(boolean isTight) {
        this.isTight = isTight;
    }

    @Override
    public boolean isTight() {
        return isTight;
    }

    @Override
    public void setProtocolVersion(String protocolVersion) {
        this.protocolVersion = protocolVersion;
    }

    @Override
    public String getProtocolVersion() {
        return protocolVersion;
    }
    
//    private Socket waitForConnection(Socket oldSocket) {
//    	while(true) {
//    		try {
//    			System.out.println("Trying to connect to " + oldSocket.getInetAddress());
//				return new Socket(oldSocket.getInetAddress(), oldSocket.getPort());
//			} catch (IOException e) { /* nop */ }
//    		
//    		try {
//				Thread.sleep(RECONNECT_WAIT_TIME);
//			} catch (InterruptedException e) {
//				return null;
//			}
//    	}
//    }

	@Override
	public synchronized void restartSession() {
		cleanUpSession();
		/* DISABLE RECONNECT.
		worker.setNetworkStatus("Reconnecting...");
		workingSocket = waitForConnection(workingSocket);
		worker.setWorkingSocket(workingSocket);
		try {
			reader = new Reader(workingSocket.getInputStream());
			writer = new Writer(workingSocket.getOutputStream());
			state = new HandshakeState(this);
			handshake();
			
			sendRefreshMessage();
			senderTask = new SenderTask(messageQueue, writer, this);
			senderThread = new Thread(senderTask, "RfbSenderTask");
			senderThread.start();
			decoders.resetDecoders();
			receiverTask = new ReceiverTask(
					reader, repaintController,
					clipboardController,
					decoders, this);
			receiverThread = new Thread(receiverTask, "RfbReceiverTask");
			receiverThread.start();
			worker.setNetworkStatus("Done.");
			(new Thread() {
				public void run() {
					try {
						Thread.sleep(5000);
					} catch (InterruptedException e) { }
					worker.setNetworkStatus("");
				}
			}).start();
		} catch (IOException e) {
			e.printStackTrace();
		} catch (UnsupportedProtocolVersionException e) {
			e.printStackTrace();
		} catch (UnsupportedSecurityTypeException e) {
			e.printStackTrace();
		} catch (AuthenticationFailedException e) {
			e.printStackTrace();
		} catch (TransportException e) {
			e.printStackTrace();
		} catch (FatalException e) {
			e.printStackTrace();
		}
		*/
	}

}
