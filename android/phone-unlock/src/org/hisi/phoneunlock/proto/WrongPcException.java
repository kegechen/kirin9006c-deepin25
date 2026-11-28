package org.hisi.phoneunlock.proto;

import java.io.IOException;

/** CHAL 里的 box 解不开：对方不是配对的那台电脑。 */
public class WrongPcException extends IOException {
    public WrongPcException() {
        super("对方不是配对的电脑（电脑信息解不开）");
    }
}
