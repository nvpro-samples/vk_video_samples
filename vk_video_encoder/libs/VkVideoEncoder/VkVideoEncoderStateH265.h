/*
 * Copyright 2022 NVIDIA Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _VKVIDEOENCODER_VKVIDEOENCODERSTATEH265_H_
#define _VKVIDEOENCODER_VKVIDEOENCODERSTATEH265_H_

struct VpsH265
{
    StdVideoH265VideoParameterSet vpsInfo = {};
};

struct SpsH265 {
    SpsH265()
    {
        hrdParameters.pSubLayerHrdParametersNal = &subLayerHrdParametersNal;

        vuiInfo.pHrdParameters = &hrdParameters;

        sps.pProfileTierLevel        = &profileTierLevel;
        sps.pDecPicBufMgr            = &decPicBufMgr;
        sps.pShortTermRefPicSet      = &shortTermRefPicSet;
        sps.pSequenceParameterSetVui = &vuiInfo;
    }

    StdVideoH265SequenceParameterSet     sps = {};
    StdVideoH265DecPicBufMgr             decPicBufMgr = {};
    StdVideoH265HrdParameters            hrdParameters = {};
    StdVideoH265ProfileTierLevel         profileTierLevel = {};
    StdVideoH265ShortTermRefPicSet       shortTermRefPicSet = {};
    StdVideoH265LongTermRefPicsSps       longTermRefPicsSps = {};
    StdVideoH265SequenceParameterSetVui  vuiInfo = {};
    StdVideoH265SubLayerHrdParameters    subLayerHrdParametersNal = {};
};

#endif /* _VKVIDEOENCODER_VKVIDEOENCODERSTATEH265_H_ */
