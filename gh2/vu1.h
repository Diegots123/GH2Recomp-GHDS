#pragma once

class PS2Runtime;

namespace ghr
{
    // Puts PCSX2's microVU in as VU1 when it is built in. Called before the
    // runtime initializes (it places VU1's memory).
    void installVu1(PS2Runtime &runtime);
}
