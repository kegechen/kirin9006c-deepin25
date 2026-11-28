package org.hisi.phoneunlock.proto;

import java.io.IOException;

/** 电脑回了 ERR <code> <msg_b64>。 */
public class PeerException extends IOException {
    public final int code;
    /** 电脑给的说明（已解 Base64，可能为空）。 */
    public final String peerMsg;

    public PeerException(int code, String peerMsg) {
        super("ERR " + code + (peerMsg == null || peerMsg.isEmpty() ? "" : " " + peerMsg));
        this.code = code;
        this.peerMsg = peerMsg == null ? "" : peerMsg;
    }
}
