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

package com.glavsoft.rfb.client;

import com.glavsoft.exceptions.TransportException;
import com.glavsoft.transport.Writer;

public class FramebufferUpdateAckMessage implements ClientToServerMessage {
	private final int sequenceNumber;

	public FramebufferUpdateAckMessage(int sequenceNumber) {
		this.sequenceNumber = sequenceNumber;
	}

	@Override
	public void send(Writer writer) throws TransportException {
		writer.write(FRAMEBUFFER_UPDATE_ACK);
		writer.writeByte(0); writer.writeInt16(0); // padding
		writer.write(sequenceNumber);
		writer.flush();
	}

	@Override
	public String toString() {
		return "FramebufferUpdateAckMessage: [sequenceNumber: " + sequenceNumber +"]";
	}
}
