# GOP Structure Edge Case and Fix

## Overview

This document describes the fixes applied to `VkVideoGopStructure::GetPositionInGOP()`:

1. **PR #191** Fixes `encodeOrder`, `numBFrames`, `bFramePos`, and `FLAGS_CLOSE_GOP` on IDR
2. **Edge Case Fix**: Fixes missing `FLAGS_CLOSE_GOP` on P-frames that naturally precede IDR

**File**: `vk_video_encoder/libs/VkVideoEncoder/VkVideoGopStructure.h`

---

## New commit commit on top of the current fixes:


Edge case fix: FLAGS_CLOSE_GOP on P before IDR |

---

## Problem Description

For certain combinations of GOP parameters, the following values were incorrectly calculated:
- `encodeOrder` - the order in which frames are encoded
- `numBFrames` - the number of B-frames in the current segment
- `bFramePos` - the position of a B-frame within its segment

### Test Configuration
```
GOP frame count: 11
IDR period: 25
Consecutive B frames: 3
GOP type: Open GOP
```

---

## Before Fix - Buggy Output

```
Frame Index:     0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15  16  17  18  19  20  21  22  23  24  25  26  27  28  29
Frame Type:    IDR   B   B   B   P   B   B   B   P   B   B   I   B   B   B   P   B   B   B   P   B   B   I   B   P IDR   B   B   B   P
Encode  order:   0   2   3   4   1   6   7   8   5  10  11   8  13  14  15  12  17  18  19  16  21  22  19  24  23   0   2   3   4   1
                                                         ↑                                           ↑
                                                    DUPLICATE!                                  DUPLICATE!
numBFrames:      X   3   3   3   X   3   3   3   X   3   3   X   3   3   3   X   3   3   3   X   3   3   X   1   X   X   3   3   3   X
                                                  ↑   ↑                                       ↑   ↑
                                              Should be 2                                 Should be 2
bFramePos:       X   0   1   2   X   0   1   2   X   0   1   X   X   0   1   X   X   0   1   X   X   0   X   0   X   X   0   1   2   X
                                                         ↑   ↑   ↑       ↑   ↑   ↑       ↑   ↑
                                                              WRONG VALUES
closeGOP:        C   -   -   -   -   -   -   -   -   -   -   -   -   -   -   -   -   -   -   -   -   -   -   -   C   C   -   -   -   -
                 ↑                                                                                               ↑
            IDR should NOT have FLAGS_CLOSE_GOP                                                             IDR wrong!
```

### Issues identified above

1. **Encode Order Duplicates**: Frame 11 and 22 had duplicate encodeOrder values
2. **numBFrames Incorrect**: Frames 9,10,20,21 showed 3 instead of 2
3. **bFramePos Wrong**: Frames 12-14, 16-18, 20-21 had wrong positions
4. **FLAGS_CLOSE_GOP on IDR**: IDR frames incorrectly had this flag set

---

## After Fix - Correct Output

```
Frame Index:     0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15  16  17  18  19  20  21  22  23  24  25  26  27  28  29
Frame Type:    IDR   B   B   B   P   B   B   B   P   B   B   I   B   B   B   P   B   B   B   P   B   B   I   B   P IDR   B   B   B   P
Encode  order:   0   2   3   4   1   6   7   8   5  10  11   9  13  14  15  12  17  18  19  16  21  22  20  24  23   0   2   3   4   1
                                                         ↑                                           ↑
                                                     FIXED (9)                                   FIXED (20)
numBFrames:      X   3   3   3   X   3   3   3   X   2   2   X   3   3   3   X   3   3   3   X   2   2   X   1   X   X   3   3   3   X
                                                  ↑   ↑                                       ↑   ↑
                                                FIXED (2)                                   FIXED (2)
bFramePos:       X   0   1   2   X   0   1   2   X   0   1   X   0   1   2   X   0   1   2   X   0   1   X   0   X   X   0   1   2   X
                                                             ↑   ↑   ↑       ↑   ↑   ↑       ↑   ↑
                                                                  ALL FIXED
closeGOP:        -   -   -   -   -   -   -   -   -   -   -   -   -   -   -   -   -   -   -   -   -   -   -   -   C   -   -   -   -   -
                 ↑                                                                                               ↑
            FIXED (no flag)                                                                                 FIXED (no flag)
```

---

## PR #191 Fix Details

PR #191 refactors the GOP structure calculation with improved documentation:

### Key Changes

1. **consecutiveBFrameCount Calculation**
   - For I or P frames: equals the number of consecutive B-frames that immediately precede this frame
   - For B frames: represents the size of the consecutive B-frame group containing this frame

2. **B-frame Promotion Logic**
   - A B-frame is upgraded to P-frame when:
     - It is the final frame in the entire sequence
     - It is the final frame within the current IDR period
     - It is the final frame in a closed GOP

3. **numBFrames Calculation for B-frames**
   - Computed as the sum of distance to previous reference + distance to next reference
   - Next reference is the nearest among: sequence end, IDR period end, closed GOP end, or min/sub-GOP start

4. **FLAGS_CLOSE_GOP Handling**
   - Removed from IDR frames (IDR is the FIRST frame of a GOP, not the last)
   - Set on promoted B-frames before IDR or closed GOP boundary

---

## Edge Case Fix Details

### Issue Discovery

When testing with aligned GOP and IDR periods (e.g., GOP=10, IDR=10), PR #191 still has one edge case bug:

```
GOP frame count: 10, IDR period: 10, Consecutive B frames: 2, Open GOP

Frame Index:     0   1   2   3   4   5   6   7   8   9  10  11  ...
Frame Type:    IDR   B   B   P   B   B   P   B   B   P IDR   B  ...

PR #191 closeGOP:  -   -   -   -   -   -   -   -   -   -   -   -  ...
                                              ↑
                                         MISSING! (Frame 9)
```

Frame 9 is a P-frame that is the last reference before IDR at frame 10. It should have `FLAGS_CLOSE_GOP`, but PR #191 doesn't set it.

### Root Cause

PR #191's `FLAGS_CLOSE_GOP` logic is only in the B-frame promotion path. When a P-frame **naturally** lands on the position just before IDR (based on the B-frame pattern, not promoted from B-frame), the flag was never set.

### Fix Applied

Added explicit boundary detection in the reference frame path:

```cpp
// Edge case fix: When a P-frame naturally falls on the position just before
// an IDR boundary (based on the B-frame pattern, not promoted from B-frame),
// it should still get FLAGS_CLOSE_GOP.
if ((m_idrPeriod > 0) && ((gopState.positionInInputOrder + 1) % m_idrPeriod == 0)) {
    gopPos.flags |= FLAGS_CLOSE_GOP;
} else if (m_closedGop && (gopPos.inGop == (m_gopFrameCount - 1))) {
    gopPos.flags |= FLAGS_CLOSE_GOP;
}
```

### After Edge Case Fix

```
Frame Index:     0   1   2   3   4   5   6   7   8   9  10  11  ...
closeGOP:        -   -   -   -   -   -   -   -   -   C   -   -  ...
                                              ↑
                                           FIXED!
```

---

## Test Cases Verified

### Primary Test Case (PR #191)
- GOP=11, IDR=25, B=3, Open GOP ✅

### Edge Cases
| Test | Configuration | Result |
|------|--------------|--------|
| No B-frames | GOP=8, IDR=16, B=0 | ✅ |
| Single B-frame | GOP=8, IDR=16, B=1 | ✅ |
| GOP = IDR period | GOP=10, IDR=10, B=2 | ✅ |
| Small GOP | GOP=4, IDR=12, B=2 | ✅ |

---

## Test Program

A test program is available at:
```
vk_video_encoder/test/gop_structure_test.cpp
```

Build and run:
```bash
cd vk_video_encoder
g++ -std=c++17 -I./include -I./libs/VkVideoEncoder \
    test/gop_structure_test.cpp \
    libs/VkVideoEncoder/VkVideoGopStructure.cpp \
    -o test/gop_structure_test
./test/gop_structure_test
```
