/*
 *      Copyright (C) 2010-2014 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "Application.h"
#include "settings/MediaSettings.h"

#include "DllAvCodec.h"
#include "cores/AudioEngine/Engines/ActiveAE/ActiveAEBuffer.h"
#include "cores/AudioEngine/Engines/ActiveAE/ActiveAEResample.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "cores/IPlayer.h"

#include "ActiveAEDSPProcess.h"
#include "ActiveAEDSPMode.h"

using namespace std;
using namespace ADDON;
using namespace ActiveAE;

#define MIN_DSP_ARRAY_SIZE 4096

CActiveAEDSPProcess::CActiveAEDSPProcess(unsigned int streamId)
 : m_StreamId(streamId)
{
  m_ChannelLayoutIn         = 0;      /* Undefined input channel layout */
  m_ChannelLayoutOut        = 0;      /* Undefined output channel layout */
  m_StreamType              = AE_DSP_ASTREAM_INVALID;
  m_NewStreamType           = AE_DSP_ASTREAM_INVALID;
  m_NewMasterMode           = AE_DSP_MASTER_MODE_ID_INVALID;
  m_ForceInit               = false;
  m_Addon_InputResample     = NULL;
  m_Addon_MasterProc        = NULL;
  m_Addon_OutputResample    = NULL;

  /*!
   * Create predefined process arrays on every supported channel for audio dsp's.
   * All are set if used or not for safety reason and unsued ones can be used from
   * dsp addons as buffer arrays.
   * If a bigger size is neeeded it becomes reallocated during DSP processing.
   */
  m_AudioInArraySize          = MIN_DSP_ARRAY_SIZE;
  m_InputResampleArraySize    = MIN_DSP_ARRAY_SIZE;
  m_MasterArraySize           = MIN_DSP_ARRAY_SIZE;
  m_PostProcessArraySize      = MIN_DSP_ARRAY_SIZE;
  m_PostProcessArrayTogglePtr = 0;
  m_OutputResampleArraySize   = MIN_DSP_ARRAY_SIZE;
  for (int i = 0; i < AE_DSP_CH_MAX; i++)
  {
    m_AudioInArray[i]         = (float*)calloc(m_AudioInArraySize, sizeof(float));
    m_InputResampleArray[i]   = (float*)calloc(m_InputResampleArraySize, sizeof(float));
    m_MasterArray[i]          = (float*)calloc(m_MasterArraySize, sizeof(float));
    m_PostProcessArray[0][i]  = (float*)calloc(m_PostProcessArraySize, sizeof(float));
    m_PostProcessArray[1][i]  = (float*)calloc(m_PostProcessArraySize, sizeof(float));
    m_OutputResampleArray[i]  = (float*)calloc(m_OutputResampleArraySize, sizeof(float));
  }
}

CActiveAEDSPProcess::~CActiveAEDSPProcess()
{
  ResetStreamFunctionsSelection();

  /* Clear the buffer arrays */
  for (int i = 0; i < AE_DSP_CH_MAX; i++)
  {
    if(m_AudioInArray[i])
      free(m_AudioInArray[i]);
    if(m_InputResampleArray[i])
      free(m_InputResampleArray[i]);
    if(m_MasterArray[i])
      free(m_MasterArray[i]);
    if(m_PostProcessArray[0][i])
      free(m_PostProcessArray[0][i]);
    if(m_PostProcessArray[1][i])
      free(m_PostProcessArray[1][i]);
    if(m_OutputResampleArray[i])
      free(m_OutputResampleArray[i]);
  }
}

void CActiveAEDSPProcess::ResetStreamFunctionsSelection()
{
  Destroy();

  m_NewMasterMode         = AE_DSP_MASTER_MODE_ID_INVALID;
  m_NewStreamType         = AE_DSP_ASTREAM_INVALID;
  m_Addon_InputResample   = NULL;
  m_Addon_MasterProc      = NULL;
  m_Addon_OutputResample  = NULL;

  m_Addons_PreProc.clear();
  m_Addons_PostProc.clear();
  m_usedMap.clear();
}

bool CActiveAEDSPProcess::Create(AEAudioFormat inputFormat, AEAudioFormat outputFormat, bool upmix, AEQuality quality, AE_DSP_STREAMTYPE iStreamType)
{
  m_InputFormat       = inputFormat;
  m_OutputFormat      = outputFormat;
  m_OutputSamplerate  = m_InputFormat.m_sampleRate;         /*!< If no resampler addon is present output samplerate is the same as input */
  m_ResampleQuality   = quality;
  m_dataFormat        = AE_FMT_FLOAT;
  m_ActiveMode        = AE_DSP_MASTER_MODE_ID_PASSOVER;     /*!< Reset the pointer for m_MasterModes about active master process, set here during mode selection */

  CSingleLock lock(m_restartSection);

  CLog::Log(LOGDEBUG, "ActiveAE DSP - %s - Audio DSP processing id %d created:", __FUNCTION__, m_StreamId);

  ResetStreamFunctionsSelection();

  CFileItem currentFile(g_application.CurrentFileItem());

  m_StreamTypeDetected = DetectStreamType(&currentFile);
  m_StreamTypeAsked    = iStreamType;

  if (iStreamType == AE_DSP_ASTREAM_AUTO)
    m_StreamType = m_StreamTypeDetected;
  else if (iStreamType >= AE_DSP_ASTREAM_BASIC || iStreamType < AE_DSP_ASTREAM_AUTO)
    m_StreamType = iStreamType;
  else
  {
    CLog::Log(LOGERROR, "ActiveAE DSP - %s - Unknown audio stream type, falling back to basic", __FUNCTION__);
    m_StreamType = AE_DSP_ASTREAM_BASIC;
  }

  /*!
   * Set general stream infos about the here processed stream
   */

  if (g_application.m_pPlayer->GetAudioStreamCount() > 0)
  {
    int identifier = CMediaSettings::Get().GetCurrentVideoSettings().m_AudioStream;
    if(identifier < 0)
      identifier = g_application.m_pPlayer->GetAudioStream();
    if (identifier < 0)
      identifier = 0;

    SPlayerAudioStreamInfo info;
    g_application.m_pPlayer->GetAudioStreamInfo(identifier, info);

    m_AddonStreamProperties.strName       = info.name.c_str();
    m_AddonStreamProperties.strLanguage   = info.language.c_str();
    m_AddonStreamProperties.strCodecId    = info.audioCodecName.c_str();
    m_AddonStreamProperties.iIdentifier   = identifier;
  }
  else
  {
    m_AddonStreamProperties.strName       = "Unknown";
    m_AddonStreamProperties.strLanguage   = "";
    m_AddonStreamProperties.strCodecId    = "";
    m_AddonStreamProperties.iIdentifier   = m_StreamId;
  }

  m_AddonStreamProperties.iStreamID       = m_StreamId;
  m_AddonStreamProperties.iStreamType     = m_StreamType;
  m_AddonStreamProperties.iChannels       = m_InputFormat.m_channelLayout.Count();
  m_AddonStreamProperties.iSampleRate     = m_InputFormat.m_sampleRate;
  m_AddonStreamProperties.iBaseType       = GetBaseType(&m_AddonStreamProperties);
  CreateStreamProfile();

  /*!
   * Set exact input and output format settings
   */
  m_AddonSettings.iStreamID               = m_StreamId;
  m_AddonSettings.iStreamType             = m_StreamType;
  m_AddonSettings.lInChannelPresentFlags  = 0;
  m_AddonSettings.iInChannels             = m_InputFormat.m_channelLayout.Count();
  m_AddonSettings.iInFrames               = m_InputFormat.m_frames;
  m_AddonSettings.iInSamplerate           = m_InputFormat.m_sampleRate;           /* The basic input samplerate from stream source */
  m_AddonSettings.iProcessFrames          = m_InputFormat.m_frames;
  m_AddonSettings.iProcessSamplerate      = m_InputFormat.m_sampleRate;           /* Default the same as input samplerate,  if resampler is present it becomes corrected */
  m_AddonSettings.lOutChannelPresentFlags = 0;
  m_AddonSettings.iOutChannels            = m_OutputFormat.m_channelLayout.Count();
  m_AddonSettings.iOutFrames              = m_OutputFormat.m_frames;
  m_AddonSettings.iOutSamplerate          = m_OutputFormat.m_sampleRate;          /* The required sample rate for pass over resampling on ActiveAEResample */
  m_AddonSettings.bStereoUpmix            = upmix;
  m_AddonSettings.bInputResamplingActive  = false;
  m_AddonSettings.iQualityLevel           = quality;

  if (m_InputFormat.m_channelLayout.HasChannel(AE_CH_FL))   m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_FL;
  if (m_InputFormat.m_channelLayout.HasChannel(AE_CH_FR))   m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_FR;
  if (m_InputFormat.m_channelLayout.HasChannel(AE_CH_FC))   m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_FC;
  if (m_InputFormat.m_channelLayout.HasChannel(AE_CH_LFE))  m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_LFE;
  if (m_InputFormat.m_channelLayout.HasChannel(AE_CH_BL))   m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_BL;
  if (m_InputFormat.m_channelLayout.HasChannel(AE_CH_BR))   m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_BR;
  if (m_InputFormat.m_channelLayout.HasChannel(AE_CH_FLOC)) m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_FLOC;
  if (m_InputFormat.m_channelLayout.HasChannel(AE_CH_FROC)) m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_FROC;
  if (m_InputFormat.m_channelLayout.HasChannel(AE_CH_BC))   m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_BC;
  if (m_InputFormat.m_channelLayout.HasChannel(AE_CH_SL))   m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_SL;
  if (m_InputFormat.m_channelLayout.HasChannel(AE_CH_SR))   m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_SR;
  if (m_InputFormat.m_channelLayout.HasChannel(AE_CH_TFL))  m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_TFL;
  if (m_InputFormat.m_channelLayout.HasChannel(AE_CH_TFR))  m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_TFR;
  if (m_InputFormat.m_channelLayout.HasChannel(AE_CH_TFC))  m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_TFC;
  if (m_InputFormat.m_channelLayout.HasChannel(AE_CH_TC))   m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_TC;
  if (m_InputFormat.m_channelLayout.HasChannel(AE_CH_TBL))  m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_TBL;
  if (m_InputFormat.m_channelLayout.HasChannel(AE_CH_TBR))  m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_TBR;
  if (m_InputFormat.m_channelLayout.HasChannel(AE_CH_TBC))  m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_TBC;
  if (m_InputFormat.m_channelLayout.HasChannel(AE_CH_BLOC)) m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_BLOC;
  if (m_InputFormat.m_channelLayout.HasChannel(AE_CH_BROC)) m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_BROC;

  if (m_OutputFormat.m_channelLayout.HasChannel(AE_CH_FL))   m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_FL;
  if (m_OutputFormat.m_channelLayout.HasChannel(AE_CH_FR))   m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_FR;
  if (m_OutputFormat.m_channelLayout.HasChannel(AE_CH_FC))   m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_FC;
  if (m_OutputFormat.m_channelLayout.HasChannel(AE_CH_LFE))  m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_LFE;
  if (m_OutputFormat.m_channelLayout.HasChannel(AE_CH_BL))   m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_BL;
  if (m_OutputFormat.m_channelLayout.HasChannel(AE_CH_BR))   m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_BR;
  if (m_OutputFormat.m_channelLayout.HasChannel(AE_CH_FLOC)) m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_FLOC;
  if (m_OutputFormat.m_channelLayout.HasChannel(AE_CH_FROC)) m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_FROC;
  if (m_OutputFormat.m_channelLayout.HasChannel(AE_CH_BC))   m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_BC;
  if (m_OutputFormat.m_channelLayout.HasChannel(AE_CH_SL))   m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_SL;
  if (m_OutputFormat.m_channelLayout.HasChannel(AE_CH_SR))   m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_SR;
  if (m_OutputFormat.m_channelLayout.HasChannel(AE_CH_TFL))  m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_TFL;
  if (m_OutputFormat.m_channelLayout.HasChannel(AE_CH_TFR))  m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_TFR;
  if (m_OutputFormat.m_channelLayout.HasChannel(AE_CH_TFC))  m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_TFC;
  if (m_OutputFormat.m_channelLayout.HasChannel(AE_CH_TC))   m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_TC;
  if (m_OutputFormat.m_channelLayout.HasChannel(AE_CH_TBL))  m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_TBL;
  if (m_OutputFormat.m_channelLayout.HasChannel(AE_CH_TBR))  m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_TBR;
  if (m_OutputFormat.m_channelLayout.HasChannel(AE_CH_TBC))  m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_TBC;
  if (m_OutputFormat.m_channelLayout.HasChannel(AE_CH_BLOC)) m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_BLOC;
  if (m_OutputFormat.m_channelLayout.HasChannel(AE_CH_BROC)) m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_BROC;

  /*!
   * Now ask all available addons about the format that it is supported and becomes selected.
   */
  AE_DSP_ADDONMAP addonMap;
  if (CActiveAEDSP::Get().GetEnabledAudioDSPAddons(addonMap) > 0)
  {
    int foundInputResamplerId = -1; /* Used to prevent double call of StreamCreate if input stream resampling is together with outer processing types */

    /* First find input resample addons to become information about processing sample rate and
     * load one allowed before master processing & final resample addon
     */
    CLog::Log(LOGDEBUG, "  ---- DSP input resample addon ---");
    int selAddonId = CMediaSettings::Get().GetCurrentAudioSettings().m_InputResampleAddon;
    for (AE_DSP_ADDONMAP_ITR itr = addonMap.begin(); itr != addonMap.end(); itr++)
    {
      AE_DSP_ADDON addon = itr->second;
      if (addon->Enabled() && addon->SupportsInputResample() && (selAddonId == -1 || selAddonId == addon->GetID()))
      {
        AE_DSP_ERROR err = addon->StreamCreate(&m_AddonSettings, &m_AddonStreamProperties);
        if (err == AE_DSP_ERROR_IGNORE_ME)
        {
          continue;
        }
        else if (err != AE_DSP_ERROR_NO_ERROR)
        {
          CLog::Log(LOGERROR, "ActiveAE DSP - %s - input resample addon creation failed on %s with %s", __FUNCTION__, addon->GetAudioDSPName().c_str(), CActiveAEDSPAddon::ToString(err));
          continue;
        }

        int processSamplerate = addon->InputResampleSampleRate(m_StreamId);
        if (processSamplerate <= 0)
        {
          CLog::Log(LOGERROR, "ActiveAE DSP - %s - input resample addon %s return invalid samplerate and becomes disabled", __FUNCTION__, addon->GetFriendlyName().c_str());
          CMediaSettings::Get().GetCurrentAudioSettings().m_InputResampleAddon = -1;
          break;
        }

        CLog::Log(LOGDEBUG, "  | - %s with resampling from %i to %i", addon->GetAudioDSPName().c_str(), m_InputFormat.m_sampleRate, processSamplerate);

        m_Addon_InputResample                     = addon->GetAudioDSPFunctionStruct();
        m_OutputSamplerate                      = processSamplerate;                  /*!< overwrite output sample rate with the new rate */
        m_AddonSettings.iProcessSamplerate      = processSamplerate;                  /*!< the processing sample rate required for all behind called processes */
        m_AddonSettings.iProcessFrames          = (int) ceil((1.0 * m_AddonSettings.iProcessSamplerate) / m_AddonSettings.iInSamplerate * m_AddonSettings.iInFrames);
        m_AddonSettings.bInputResamplingActive  = true;
        foundInputResamplerId                   = addon->GetID();

        m_usedMap.insert(std::make_pair(foundInputResamplerId, addon));
        CMediaSettings::Get().GetCurrentAudioSettings().m_InputResampleAddon = foundInputResamplerId;
        break;
      }
    }
    if (foundInputResamplerId <= 0)
      CLog::Log(LOGDEBUG, "  | - no input resample addon present or enabled");

    /* Now init all other dsp relavant addons
     */
    for (AE_DSP_ADDONMAP_ITR itr = addonMap.begin(); itr != addonMap.end(); itr++)
    {
      AE_DSP_ADDON addon = itr->second;
      if (addon->Enabled() && addon->GetID() != foundInputResamplerId)
      {
        AE_DSP_ERROR err = addon->StreamCreate(&m_AddonSettings, &m_AddonStreamProperties);
        if (err == AE_DSP_ERROR_NO_ERROR)
        {
          m_usedMap.insert(std::make_pair(addon->GetID(), addon));
        }
        else if (err == AE_DSP_ERROR_IGNORE_ME)
          continue;
        else
          CLog::Log(LOGERROR, "ActiveAE DSP - %s - addon creation failed on %s with %s", __FUNCTION__, addon->GetAudioDSPName().c_str(), CActiveAEDSPAddon::ToString(err));
      }
    }
  }

  if (m_usedMap.size() == 0)
  {
    CLog::Log(LOGERROR, "ActiveAE DSP - %s - no usable addons present", __FUNCTION__);
    return false;
  }

  /*!
   * Load all required pre process dsp addons
   */
  CLog::Log(LOGDEBUG, "  ---- DSP pre process addons ---");
  for (AE_DSP_ADDONMAP_ITR itr = m_usedMap.begin(); itr != m_usedMap.end(); itr++)
  {
    AE_DSP_ADDON addon = itr->second;
    if (addon->SupportsPreProcessing())
    {
      CLog::Log(LOGDEBUG, "  | - %s", addon->GetAudioDSPName().c_str());
      m_Addons_PreProc.push_back(addon->GetAudioDSPFunctionStruct());
    }
  }
  if (m_Addons_PreProc.empty())
    CLog::Log(LOGDEBUG, "  | - no pre processing addon's present or enabled");

  /*!
   * Setup off mode, used if dsp master processing is set off, required to have data
   * for stream information functions.
   */
  CActiveAEDSPModePtr mode = CActiveAEDSPModePtr(new CActiveAEDSPMode((AE_DSP_BASETYPE)m_AddonStreamProperties.iBaseType));
  m_MasterModes.push_back(mode);
  m_ActiveMode = AE_DSP_MASTER_MODE_ID_PASSOVER;

  /*!
   * Load all available master modes from addons and put together with database
   */
  CLog::Log(LOGDEBUG, "  ---- DSP master processing addons ---");
  for (AE_DSP_ADDONMAP_ITR itr = m_usedMap.begin(); itr != m_usedMap.end(); itr++)
  {
    AE_DSP_ADDON addon = itr->second;
    if (addon->SupportsMasterProcess())
    {
      AE_DSP_MASTER_MODES modes;
      AE_DSP_ERROR err = addon->MasterProcessGetModes(m_StreamId, modes);
      if (err == AE_DSP_ERROR_NO_ERROR)
      {
        CLog::Log(LOGDEBUG, "  | - %s", addon->GetAudioDSPName().c_str());
        CLog::Log(LOGDEBUG, "  | ---- with %i modes ---", modes.iModesCount);
        for (unsigned int i = 0; i < modes.iModesCount; i++)
        {
          CActiveAEDSPModePtr mode = CActiveAEDSPModePtr(new CActiveAEDSPMode(modes.mode[i], addon->GetID()));
          if (mode->AddUpdate() > AE_DSP_MASTER_MODE_ID_PASSOVER)
          {
            CLog::Log(LOGDEBUG, "  | -- %s ModeID='%i'", mode->AddonModeName().c_str(), mode->ModeID());
            mode->SetBaseType((AE_DSP_BASETYPE)m_AddonStreamProperties.iBaseType);
            m_MasterModes.push_back(mode);
          }
          else
          {
            CLog::Log(LOGERROR, "ActiveAE DSP - %s - Add or update of '%s' with %s ModeID='%i' to database failed", __FUNCTION__, addon->GetAudioDSPName().c_str(), mode->AddonModeName().c_str(), mode->ModeID());
          }
        }
      }
      else if (err != AE_DSP_ERROR_IGNORE_ME)
        CLog::Log(LOGERROR, "ActiveAE DSP - %s - load of available master process modes failed on %s with %s", __FUNCTION__, addon->GetAudioDSPName().c_str(), CActiveAEDSPAddon::ToString(err));
    }
  }

  /* Get selected source for current input */
  CLog::Log(LOGDEBUG, "  | ---- enabled ---");

  int ModeID = CMediaSettings::Get().GetCurrentAudioSettings().m_MasterModes[m_AddonStreamProperties.iStreamType][m_AddonStreamProperties.iBaseType];
  for (unsigned int ptr = 0; ptr < m_MasterModes.size(); ptr++)
  {
    CActiveAEDSPModePtr mode = m_MasterModes.at(ptr);
    if (!mode->IsHidden() && mode->ModeID() != AE_DSP_MASTER_MODE_ID_PASSOVER)
    {
      if (mode->IsPrimary())
      {
        if (ModeID == AE_DSP_MASTER_MODE_ID_INVALID)
        {
          m_Addon_MasterProc = m_usedMap[mode->AddonID()]->GetAudioDSPFunctionStruct();
          m_ActiveMode = (int)ptr;
          CMediaSettings::Get().GetCurrentAudioSettings().m_MasterModes[m_AddonStreamProperties.iStreamType][m_AddonStreamProperties.iBaseType] = mode->ModeID();
          CLog::Log(LOGDEBUG, "  | -- %s (as default)", mode->AddonModeName().c_str());
          break;
        }
      }

      if (ModeID == mode->ModeID())
      {
        m_Addon_MasterProc = m_usedMap[mode->AddonID()]->GetAudioDSPFunctionStruct();
        m_ActiveMode = (int)ptr;
        CLog::Log(LOGDEBUG, "  | -- %s (selected)", mode->AddonModeName().c_str());
        break;
      }
    }
  }

  /*!
   * Setup the one allowed master processing addon and inform addon about selected mode
   */
  if (m_Addon_MasterProc)
  {
    try
    {
      AE_DSP_ERROR err = m_Addon_MasterProc->MasterProcessSetMode(m_StreamId, m_AddonStreamProperties.iStreamType, m_MasterModes[m_ActiveMode]->AddonModeNumber(), m_MasterModes[m_ActiveMode]->ModeID());
      if (err != AE_DSP_ERROR_NO_ERROR)
      {
        CLog::Log(LOGERROR, "ActiveAE DSP - %s - addon master mode selection failed on %s with Mode '%s' with %s",
                                __FUNCTION__,
                                m_usedMap[m_MasterModes[m_ActiveMode]->AddonID()]->GetAudioDSPName().c_str(),
                                m_MasterModes[m_ActiveMode]->AddonModeName().c_str(),
                                CActiveAEDSPAddon::ToString(err));
        m_Addon_MasterProc = NULL;
        m_ActiveMode = AE_DSP_MASTER_MODE_ID_PASSOVER;
      }
    }
    catch (exception &e)
    {
      CLog::Log(LOGERROR, "ActiveAE DSP - exception '%s' caught while trying to call 'MasterProcessSetMode' on add-on '%s'. Please contact the developer of this add-on",
                              e.what(),
                              m_usedMap[m_MasterModes[m_ActiveMode]->AddonID()]->GetAudioDSPName().c_str());
      m_Addon_MasterProc = NULL;
      m_ActiveMode = AE_DSP_MASTER_MODE_ID_PASSOVER;
    }
  }
  else
  {
    CLog::Log(LOGDEBUG, "  | -- No master process selected!");
  }

  /*!
   * Load all required post process dsp addons
   */
  CLog::Log(LOGDEBUG, "  ---- DSP post process addons ---");
  for (AE_DSP_ADDONMAP_ITR itr = m_usedMap.begin(); itr != m_usedMap.end(); itr++)
  {
    AE_DSP_ADDON addon = itr->second;
    if (addon->SupportsPostProcess())
    {
      CLog::Log(LOGDEBUG, "  | - %s", addon->GetAudioDSPName().c_str());
      m_Addons_PostProc.push_back(addon->GetAudioDSPFunctionStruct());
    }
  }
  if (m_Addons_PostProc.empty())
    CLog::Log(LOGDEBUG, "  | - no post processing addon's present or enabled");

  /*!
   * Load one allowed addon for resampling after final post processing
   */
  CLog::Log(LOGDEBUG, "  ---- DSP post resample addon ---");
  if (m_AddonSettings.iProcessSamplerate != m_OutputFormat.m_sampleRate)
  {
    for (AE_DSP_ADDONMAP_ITR itr = m_usedMap.begin(); itr != m_usedMap.end(); itr++)
    {
      AE_DSP_ADDON addon = itr->second;
      int selAddonId = CMediaSettings::Get().GetCurrentAudioSettings().m_OutputResampleAddon;
      if (addon->SupportsOutputResample() && (selAddonId == -1 || selAddonId == addon->GetID()))
      {
        int outSamplerate = addon->OutputResampleSampleRate(m_StreamId);
        if (outSamplerate > 0)
        {
          CLog::Log(LOGDEBUG, "  | - %s with resampling to %i", addon->GetAudioDSPName().c_str(), outSamplerate);
          CMediaSettings::Get().GetCurrentAudioSettings().m_OutputResampleAddon = addon->GetID();
          m_Addon_OutputResample = addon->GetAudioDSPFunctionStruct();
          m_OutputSamplerate   = outSamplerate;
        }
        else
        {
          CLog::Log(LOGERROR, "ActiveAE DSP - %s - post resample addon %s return invalid samplerate and becomes disabled", __FUNCTION__, addon->GetFriendlyName().c_str());
          CMediaSettings::Get().GetCurrentAudioSettings().m_OutputResampleAddon = -1;
        }
        break;
      }
    }
    if (m_Addon_OutputResample == NULL)
      CLog::Log(LOGDEBUG, "  | - no final post resample addon present or enabled");
  }
  else
  {
    CLog::Log(LOGDEBUG, "  | - no final resampling needed, process and final samplerate the same");
  }


  if (CLog::GetLogLevel() == LOGDEBUG) // Speed improve
  {
    CLog::Log(LOGDEBUG, "  ----  Input stream  ----");
    CLog::Log(LOGDEBUG, "  | Identifier           : %d", m_AddonStreamProperties.iIdentifier);
    CLog::Log(LOGDEBUG, "  | Stream Type          : %s", m_AddonStreamProperties.iStreamType == AE_DSP_ASTREAM_BASIC   ? "Basic"   :
                                                         m_AddonStreamProperties.iStreamType == AE_DSP_ASTREAM_MUSIC   ? "Music"   :
                                                         m_AddonStreamProperties.iStreamType == AE_DSP_ASTREAM_MOVIE   ? "Movie"   :
                                                         m_AddonStreamProperties.iStreamType == AE_DSP_ASTREAM_GAME    ? "Game"    :
                                                         m_AddonStreamProperties.iStreamType == AE_DSP_ASTREAM_APP     ? "App"     :
                                                         m_AddonStreamProperties.iStreamType == AE_DSP_ASTREAM_PHONE   ? "Phone"   :
                                                         m_AddonStreamProperties.iStreamType == AE_DSP_ASTREAM_MESSAGE ? "Message" :
                                                         "Unknown");
    CLog::Log(LOGDEBUG, "  | Name                 : %s", m_AddonStreamProperties.strName);
    CLog::Log(LOGDEBUG, "  | Language             : %s", m_AddonStreamProperties.strLanguage);
    CLog::Log(LOGDEBUG, "  | Codec                : %s", m_AddonStreamProperties.strCodecId);
    CLog::Log(LOGDEBUG, "  | Sample Rate          : %d", m_AddonStreamProperties.iSampleRate);
    CLog::Log(LOGDEBUG, "  | Channels             : %d", m_AddonStreamProperties.iChannels);
    CLog::Log(LOGDEBUG, "  ----  Input format  ----");
    CLog::Log(LOGDEBUG, "  | Sample Rate          : %d", m_InputFormat.m_sampleRate);
    CLog::Log(LOGDEBUG, "  | Sample Format        : %s", CAEUtil::DataFormatToStr(m_InputFormat.m_dataFormat));
    CLog::Log(LOGDEBUG, "  | Channel Count        : %d", m_InputFormat.m_channelLayout.Count());
    CLog::Log(LOGDEBUG, "  | Channel Layout       : %s", ((std::string)m_InputFormat.m_channelLayout).c_str());
    CLog::Log(LOGDEBUG, "  | Frames               : %d", m_InputFormat.m_frames);
    CLog::Log(LOGDEBUG, "  | Frame Samples        : %d", m_InputFormat.m_frameSamples);
    CLog::Log(LOGDEBUG, "  | Frame Size           : %d", m_InputFormat.m_frameSize);
    CLog::Log(LOGDEBUG, "  ----  Process format ----");
    CLog::Log(LOGDEBUG, "  | Sample Rate          : %d", m_AddonSettings.iProcessSamplerate);
    CLog::Log(LOGDEBUG, "  | Frames               : %d", m_AddonSettings.iProcessFrames);
    CLog::Log(LOGDEBUG, "  ----  Output format ----");
    CLog::Log(LOGDEBUG, "  | Sample Rate          : %d", m_AddonSettings.iOutSamplerate);
    CLog::Log(LOGDEBUG, "  | Sample Format        : %s", CAEUtil::DataFormatToStr(m_OutputFormat.m_dataFormat));
    CLog::Log(LOGDEBUG, "  | Channel Count        : %d", m_OutputFormat.m_channelLayout.Count());
    CLog::Log(LOGDEBUG, "  | Channel Layout       : %s", ((std::string)m_OutputFormat.m_channelLayout).c_str());
    CLog::Log(LOGDEBUG, "  | Frames               : %d", m_OutputFormat.m_frames);
    CLog::Log(LOGDEBUG, "  | Frame Samples        : %d", m_OutputFormat.m_frameSamples);
    CLog::Log(LOGDEBUG, "  | Frame Size           : %d", m_OutputFormat.m_frameSize);
  }

  m_ForceInit = true;
  return true;
}

bool CActiveAEDSPProcess::CreateStreamProfile()
{
#ifdef FF_PROFILE_AC3
  /* use profile to determine the AC3 type */
  if (m_AddonStreamProperties.iBaseType == AE_DSP_ABASE_AC3)
  {
    m_AddonStreamProperties.Profile.ac3.iChannelMode = m_InputFormat.m_profile & 0xf;

    if (m_InputFormat.m_profile & FF_PROFILE_AC3_WITH_SURROUND)
      m_AddonStreamProperties.Profile.ac3.bWithSurround = true;
    else
      m_AddonStreamProperties.Profile.ac3.bWithSurround = false;

    if (m_InputFormat.m_profile & FF_PROFILE_AC3_WITH_DD_EX)
      m_AddonStreamProperties.Profile.ac3.bWithDolbyDigitalEx = true;
    else
      m_AddonStreamProperties.Profile.ac3.bWithDolbyDigitalEx = false;

    if (m_InputFormat.m_profile & FF_PROFILE_AC3_WITH_LARGE_ROOM)
      m_AddonStreamProperties.Profile.ac3.iRoomType = AE_DSP_PROFILE_AC3_ROOM_LARGE;
    else if (m_InputFormat.m_profile & FF_PROFILE_AC3_WITH_SMALL_ROOM)
      m_AddonStreamProperties.Profile.ac3.iRoomType = AE_DSP_PROFILE_AC3_ROOM_SMALL;
    else
      m_AddonStreamProperties.Profile.ac3.iRoomType = AE_DSP_PROFILE_AC3_ROOM_UNDEFINED;

    return true;
  }
#endif
  return false;
}

void CActiveAEDSPProcess::Destroy()
{
  CSingleLock lock(m_restartSection);

  m_MasterModes.clear();

  if (!CActiveAEDSP::Get().IsActivated())
    return;

  for (AE_DSP_ADDONMAP_ITR itr = m_usedMap.begin(); itr != m_usedMap.end(); itr++)
  {
    itr->second->StreamDestroy(m_StreamId);
  }
}

void CActiveAEDSPProcess::ForceReinit()
{
  CSingleLock lock(m_restartSection);
  m_ForceInit = true;
}

AE_DSP_STREAMTYPE CActiveAEDSPProcess::DetectStreamType(const CFileItem *item)
{
  AE_DSP_STREAMTYPE detected = AE_DSP_ASTREAM_BASIC;
  if (item->HasMusicInfoTag())
    detected = AE_DSP_ASTREAM_MUSIC;
  else if (item->HasVideoInfoTag() || g_application.m_pPlayer->HasVideo())
    detected = AE_DSP_ASTREAM_MOVIE;
//    else if (item->HasVideoInfoTag())
//      detected = AE_DSP_ASTREAM_GAME;
//    else if (item->HasVideoInfoTag())
//      detected = AE_DSP_ASTREAM_APP;
//    else if (item->HasVideoInfoTag())
//      detected = AE_DSP_ASTREAM_MESSAGE;
//    else if (item->HasVideoInfoTag())
//      detected = AE_DSP_ASTREAM_PHONE;
  else
    detected = AE_DSP_ASTREAM_BASIC;

  return detected;
}

unsigned int CActiveAEDSPProcess::GetStreamId() const
{
  return m_StreamId;
}

unsigned int CActiveAEDSPProcess::GetSamplerate()
{
  return m_OutputSamplerate;
}

CAEChannelInfo CActiveAEDSPProcess::GetChannelLayout()
{
  return m_OutputFormat.m_channelLayout;
}

AEDataFormat CActiveAEDSPProcess::GetDataFormat()
{
  return m_dataFormat;
}

AEAudioFormat CActiveAEDSPProcess::GetInputFormat()
{
  return m_InputFormat;
}

AE_DSP_STREAMTYPE CActiveAEDSPProcess::GetDetectedStreamType()
{
  return m_StreamTypeDetected;
}

AE_DSP_STREAMTYPE CActiveAEDSPProcess::GetStreamType()
{
  return m_StreamType;
}

AE_DSP_STREAMTYPE CActiveAEDSPProcess::GetUsedAddonStreamType()
{
  return (AE_DSP_STREAMTYPE)m_AddonStreamProperties.iStreamType;
}

AE_DSP_BASETYPE CActiveAEDSPProcess::GetBaseType(AE_DSP_STREAM_PROPERTIES *props)
{
  if (!strcmp(props->strCodecId, "ac3"))
    return AE_DSP_ABASE_AC3;
  else if (!strcmp(props->strCodecId, "eac3"))
    return AE_DSP_ABASE_EAC3;
  else if (!strcmp(props->strCodecId, "dca") || !strcmp(props->strCodecId, "dts"))
    return AE_DSP_ABASE_DTS;
  else if (!strcmp(props->strCodecId, "dtshd_hra"))
    return AE_DSP_ABASE_DTSHD_HRA;
  else if (!strcmp(props->strCodecId, "dtshd_ma"))
    return AE_DSP_ABASE_DTSHD_MA;
  else if (!strcmp(props->strCodecId, "truehd"))
    return AE_DSP_ABASE_TRUEHD;
  else if (!strcmp(props->strCodecId, "mlp"))
    return AE_DSP_ABASE_MLP;
  else if (!strcmp(props->strCodecId, "flac"))
    return AE_DSP_ABASE_FLAC;
  else if (props->iChannels > 2)
    return AE_DSP_ABASE_MULTICHANNEL;
  else if (props->iChannels == 2)
    return AE_DSP_ABASE_STEREO;
  else
    return AE_DSP_ABASE_MONO;
}

AE_DSP_BASETYPE CActiveAEDSPProcess::GetUsedAddonBaseType()
{
  return GetBaseType(&m_AddonStreamProperties);
}

bool CActiveAEDSPProcess::GetMasterModeStreamInfoString(CStdString &strInfo)
{
  if (m_ActiveMode == AE_DSP_MASTER_MODE_ID_PASSOVER)
  {
    strInfo = "";
    return true;
  }

  if (!m_Addon_MasterProc || m_ActiveMode < 0)
    return false;

  strInfo = m_usedMap[m_MasterModes[m_ActiveMode]->AddonID()]->GetMasterModeStreamInfoString(m_StreamId);

  return true;
}

bool CActiveAEDSPProcess::GetMasterModeTypeInformation(AE_DSP_STREAMTYPE &streamTypeUsed, AE_DSP_BASETYPE &baseType, int &iModeID)
{
  streamTypeUsed  = (AE_DSP_STREAMTYPE)m_AddonStreamProperties.iStreamType;

  if (m_ActiveMode < 0)
    return false;

  baseType        = m_MasterModes[m_ActiveMode]->BaseType();
  iModeID         = m_MasterModes[m_ActiveMode]->ModeID();
  return true;
}

const char *CActiveAEDSPProcess::GetStreamTypeName(AE_DSP_STREAMTYPE iStreamType)
{
  return iStreamType == AE_DSP_ASTREAM_BASIC   ? "Basic"     :
         iStreamType == AE_DSP_ASTREAM_MUSIC   ? "Music"     :
         iStreamType == AE_DSP_ASTREAM_MOVIE   ? "Movie"     :
         iStreamType == AE_DSP_ASTREAM_GAME    ? "Game"      :
         iStreamType == AE_DSP_ASTREAM_APP     ? "App"       :
         iStreamType == AE_DSP_ASTREAM_PHONE   ? "Phone"     :
         iStreamType == AE_DSP_ASTREAM_MESSAGE ? "Message"   :
         iStreamType == AE_DSP_ASTREAM_AUTO    ? "Automatic" :
         "Unknown";
}

bool CActiveAEDSPProcess::MasterModeChange(int iModeID, AE_DSP_STREAMTYPE iStreamType)
{
  bool bReturn = false;
  bool bSwitchStreamType = iStreamType != AE_DSP_ASTREAM_INVALID;

  /* The Mode is already used and need not to set up again */
  if (m_ActiveMode >= AE_DSP_MASTER_MODE_ID_PASSOVER && m_MasterModes[m_ActiveMode]->ModeID() == iModeID && !bSwitchStreamType)
    return true;

  CSingleLock lock(m_restartSection);

  CLog::Log(LOGDEBUG, "ActiveAE DSP - %s - Audio DSP processing id %d mode change:", __FUNCTION__, m_StreamId);
  if (bSwitchStreamType && m_StreamType != iStreamType)
  {
    AE_DSP_STREAMTYPE old = m_StreamType;
    CLog::Log(LOGDEBUG, "  ----  Input stream  ----");
    if (iStreamType == AE_DSP_ASTREAM_AUTO)
      m_StreamType = m_StreamTypeDetected;
    else if (iStreamType >= AE_DSP_ASTREAM_BASIC || iStreamType < AE_DSP_ASTREAM_AUTO)
      m_StreamType = iStreamType;
    else
    {
      CLog::Log(LOGWARNING, "ActiveAE DSP - %s - Unknown audio stream type, falling back to basic", __FUNCTION__);
      m_StreamType = AE_DSP_ASTREAM_BASIC;
    }

    CLog::Log(LOGDEBUG, "  | Stream Type change   : From '%s' to '%s'", GetStreamTypeName(old), GetStreamTypeName(m_StreamType));
  }

  /*!
   * Set the new stream type to the addon settings and properties structures.
   * If the addon want to use another stream type, it can be becomes written inside
   * the m_AddonStreamProperties.iStreamType.
   */
  m_AddonStreamProperties.iStreamType = m_StreamType;
  m_AddonSettings.iStreamType         = m_StreamType;

  if (iModeID == AE_DSP_MASTER_MODE_ID_PASSOVER)
  {
    CLog::Log(LOGINFO, "ActiveAE DSP - Switching master mode off");
    m_Addon_MasterProc  = NULL;
    m_ActiveMode        = AE_DSP_MASTER_MODE_ID_PASSOVER;
    bReturn             = true;
  }
  else
  {
    CActiveAEDSPModePtr mode;
    for (unsigned int ptr = 0; ptr < m_MasterModes.size(); ptr++)
    {
      mode = m_MasterModes.at(ptr);
      if (mode->ModeID() == iModeID && !mode->IsHidden())
      {
        AudioDSP *newMasterProc = m_usedMap[mode->AddonID()]->GetAudioDSPFunctionStruct();
        try
        {
          AE_DSP_ERROR err = newMasterProc->MasterProcessSetMode(m_StreamId, m_AddonStreamProperties.iStreamType, mode->AddonModeNumber(), mode->ModeID());
          if (err != AE_DSP_ERROR_NO_ERROR)
          {
            CLog::Log(LOGERROR, "ActiveAE DSP - %s - addon master mode selection failed on %s with Mode '%s' with %s",
                                    __FUNCTION__,
                                    m_usedMap[mode->AddonID()]->GetAudioDSPName().c_str(),
                                    mode->AddonModeName().c_str(),
                                    CActiveAEDSPAddon::ToString(err));
          }
          else
          {
            if (m_ActiveMode > AE_DSP_MASTER_MODE_ID_PASSOVER)
              m_MasterModes[m_ActiveMode]->AddUpdate();

            CLog::Log(LOGINFO, "ActiveAE DSP - Switching master mode to '%s' as '%s' on '%s'",
                                    mode->AddonModeName().c_str(),
                                    GetStreamTypeName((AE_DSP_STREAMTYPE)m_AddonStreamProperties.iStreamType),
                                    m_usedMap[mode->AddonID()]->GetAudioDSPName().c_str());
            if (m_AddonStreamProperties.iStreamType != m_StreamType)
            {
              CLog::Log(LOGDEBUG, "ActiveAE DSP - Addon force stream type from '%s' to '%s'",
                                    GetStreamTypeName(m_StreamType),
                                    GetStreamTypeName((AE_DSP_STREAMTYPE)m_AddonStreamProperties.iStreamType));
            }
            m_Addon_MasterProc  = newMasterProc;
            m_ActiveMode        = (int)ptr;
            bReturn             = true;
          }
        }
        catch (exception &e)
        {
          CLog::Log(LOGERROR, "ActiveAE DSP - exception '%s' caught while trying to call 'MasterModeChange' on add-on '%s'. Please contact the developer of this add-on",
                                  e.what(),
                                  m_usedMap[mode->AddonID()]->GetAudioDSPName().c_str());
        }

        return bReturn;
      }
    }
  }
  return bReturn;
}

void CActiveAEDSPProcess::ClearArray(float **array, unsigned int samples)
{
  unsigned int presentFlag = 1;
  for (int i = 0; i < AE_DSP_CH_MAX; i++)
  {
    if (m_AddonSettings.lOutChannelPresentFlags & presentFlag)
      memset(array[i], 0, samples*sizeof(float));
    presentFlag <<= 1;
  }
}

bool CActiveAEDSPProcess::Process(CSampleBuffer *in, CSampleBuffer *out, CActiveAEResample *resampler)
{
  int ptr;

  CSingleLock lock(m_restartSection);

  bool needDSPAddonsReinit = m_ForceInit;

  /* Detect interleaved input stream channel positions if unknown or changed */
  if (m_ChannelLayoutIn != in->pkt->config.channel_layout)
  {
    m_ChannelLayoutIn = in->pkt->config.channel_layout;

    m_idx_in[AE_CH_FL]    = resampler->GetAVChannelIndex(AE_CH_FL,   m_ChannelLayoutIn);
    m_idx_in[AE_CH_FR]    = resampler->GetAVChannelIndex(AE_CH_FR,   m_ChannelLayoutIn);
    m_idx_in[AE_CH_FC]    = resampler->GetAVChannelIndex(AE_CH_FC,   m_ChannelLayoutIn);
    m_idx_in[AE_CH_LFE]   = resampler->GetAVChannelIndex(AE_CH_LFE,  m_ChannelLayoutIn);
    m_idx_in[AE_CH_BL]    = resampler->GetAVChannelIndex(AE_CH_BL,   m_ChannelLayoutIn);
    m_idx_in[AE_CH_BR]    = resampler->GetAVChannelIndex(AE_CH_BR,   m_ChannelLayoutIn);
    m_idx_in[AE_CH_FLOC]  = resampler->GetAVChannelIndex(AE_CH_FLOC, m_ChannelLayoutIn);
    m_idx_in[AE_CH_FROC]  = resampler->GetAVChannelIndex(AE_CH_FROC, m_ChannelLayoutIn);
    m_idx_in[AE_CH_BC]    = resampler->GetAVChannelIndex(AE_CH_BC,   m_ChannelLayoutIn);
    m_idx_in[AE_CH_SL]    = resampler->GetAVChannelIndex(AE_CH_SL,   m_ChannelLayoutIn);
    m_idx_in[AE_CH_SR]    = resampler->GetAVChannelIndex(AE_CH_SR,   m_ChannelLayoutIn);
    m_idx_in[AE_CH_TC]    = resampler->GetAVChannelIndex(AE_CH_TC,   m_ChannelLayoutIn);
    m_idx_in[AE_CH_TFL]   = resampler->GetAVChannelIndex(AE_CH_TFL,  m_ChannelLayoutIn);
    m_idx_in[AE_CH_TFC]   = resampler->GetAVChannelIndex(AE_CH_TFC,  m_ChannelLayoutIn);
    m_idx_in[AE_CH_TFR]   = resampler->GetAVChannelIndex(AE_CH_TFR,  m_ChannelLayoutIn);
    m_idx_in[AE_CH_TBL]   = resampler->GetAVChannelIndex(AE_CH_TBL,  m_ChannelLayoutIn);
    m_idx_in[AE_CH_TBC]   = resampler->GetAVChannelIndex(AE_CH_TBC,  m_ChannelLayoutIn);
    m_idx_in[AE_CH_TBR]   = resampler->GetAVChannelIndex(AE_CH_TBR,  m_ChannelLayoutIn);
    m_idx_in[AE_CH_BLOC]  = resampler->GetAVChannelIndex(AE_CH_BLOC, m_ChannelLayoutIn);
    m_idx_in[AE_CH_BROC]  = resampler->GetAVChannelIndex(AE_CH_BROC, m_ChannelLayoutIn);

    needDSPAddonsReinit = true;
  }

  /* Detect also interleaved output stream channel positions if unknown or changed */
  if (m_ChannelLayoutOut != out->pkt->config.channel_layout)
  {
    m_ChannelLayoutOut = out->pkt->config.channel_layout;

    m_idx_out[AE_CH_FL]   = resampler->GetAVChannelIndex(AE_CH_FL,   m_ChannelLayoutOut);
    m_idx_out[AE_CH_FR]   = resampler->GetAVChannelIndex(AE_CH_FR,   m_ChannelLayoutOut);
    m_idx_out[AE_CH_FC]   = resampler->GetAVChannelIndex(AE_CH_FC,   m_ChannelLayoutOut);
    m_idx_out[AE_CH_LFE]  = resampler->GetAVChannelIndex(AE_CH_LFE,  m_ChannelLayoutOut);
    m_idx_out[AE_CH_BL]   = resampler->GetAVChannelIndex(AE_CH_BL,   m_ChannelLayoutOut);
    m_idx_out[AE_CH_BR]   = resampler->GetAVChannelIndex(AE_CH_BR,   m_ChannelLayoutOut);
    m_idx_out[AE_CH_FLOC] = resampler->GetAVChannelIndex(AE_CH_FLOC, m_ChannelLayoutOut);
    m_idx_out[AE_CH_FROC] = resampler->GetAVChannelIndex(AE_CH_FROC, m_ChannelLayoutOut);
    m_idx_out[AE_CH_BC]   = resampler->GetAVChannelIndex(AE_CH_BC,   m_ChannelLayoutOut);
    m_idx_out[AE_CH_SL]   = resampler->GetAVChannelIndex(AE_CH_SL,   m_ChannelLayoutOut);
    m_idx_out[AE_CH_SR]   = resampler->GetAVChannelIndex(AE_CH_SR,   m_ChannelLayoutOut);
    m_idx_out[AE_CH_TC]   = resampler->GetAVChannelIndex(AE_CH_TC,   m_ChannelLayoutOut);
    m_idx_out[AE_CH_TFL]  = resampler->GetAVChannelIndex(AE_CH_TFL,  m_ChannelLayoutOut);
    m_idx_out[AE_CH_TFC]  = resampler->GetAVChannelIndex(AE_CH_TFC,  m_ChannelLayoutOut);
    m_idx_out[AE_CH_TFR]  = resampler->GetAVChannelIndex(AE_CH_TFR,  m_ChannelLayoutOut);
    m_idx_out[AE_CH_TBL]  = resampler->GetAVChannelIndex(AE_CH_TBL,  m_ChannelLayoutOut);
    m_idx_out[AE_CH_TBC]  = resampler->GetAVChannelIndex(AE_CH_TBC,  m_ChannelLayoutOut);
    m_idx_out[AE_CH_TBR]  = resampler->GetAVChannelIndex(AE_CH_TBR,  m_ChannelLayoutOut);
    m_idx_out[AE_CH_BLOC] = resampler->GetAVChannelIndex(AE_CH_BLOC, m_ChannelLayoutOut);
    m_idx_out[AE_CH_BROC] = resampler->GetAVChannelIndex(AE_CH_BROC, m_ChannelLayoutOut);

    needDSPAddonsReinit = true;
  }

  if (needDSPAddonsReinit)
  {
    m_AddonSettings.lInChannelPresentFlags = 0;
    if (m_idx_in[AE_CH_FL] >= 0)    m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_FL;
    if (m_idx_in[AE_CH_FR] >= 0)    m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_FR;
    if (m_idx_in[AE_CH_FC] >= 0)    m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_FC;
    if (m_idx_in[AE_CH_LFE] >= 0)   m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_LFE;
    if (m_idx_in[AE_CH_BL] >= 0)    m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_BL;
    if (m_idx_in[AE_CH_BR] >= 0)    m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_BR;
    if (m_idx_in[AE_CH_FLOC] >= 0)  m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_FLOC;
    if (m_idx_in[AE_CH_FROC] >= 0)  m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_FROC;
    if (m_idx_in[AE_CH_BC] >= 0)    m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_BC;
    if (m_idx_in[AE_CH_SL] >= 0)    m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_SL;
    if (m_idx_in[AE_CH_SR] >= 0)    m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_SR;
    if (m_idx_in[AE_CH_TFL] >= 0)   m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_TFL;
    if (m_idx_in[AE_CH_TFR] >= 0)   m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_TFR;
    if (m_idx_in[AE_CH_TFC] >= 0)   m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_TFC;
    if (m_idx_in[AE_CH_TC] >= 0)    m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_TC;
    if (m_idx_in[AE_CH_TBL] >= 0)   m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_TBL;
    if (m_idx_in[AE_CH_TBR] >= 0)   m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_TBR;
    if (m_idx_in[AE_CH_TBC] >= 0)   m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_TBC;
    if (m_idx_in[AE_CH_BLOC] >= 0)  m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_BLOC;
    if (m_idx_in[AE_CH_BROC] >= 0)  m_AddonSettings.lInChannelPresentFlags |= AE_DSP_PRSNT_CH_BROC;

    m_AddonSettings.lOutChannelPresentFlags = 0;
    if (m_idx_out[AE_CH_FL] >= 0)   m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_FL;
    if (m_idx_out[AE_CH_FR] >= 0)   m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_FR;
    if (m_idx_out[AE_CH_FC] >= 0)   m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_FC;
    if (m_idx_out[AE_CH_LFE] >= 0)  m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_LFE;
    if (m_idx_out[AE_CH_BL] >= 0)   m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_BL;
    if (m_idx_out[AE_CH_BR] >= 0)   m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_BR;
    if (m_idx_out[AE_CH_FLOC] >= 0) m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_FLOC;
    if (m_idx_out[AE_CH_FROC] >= 0) m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_FROC;
    if (m_idx_out[AE_CH_BC] >= 0)   m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_BC;
    if (m_idx_out[AE_CH_SL] >= 0)   m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_SL;
    if (m_idx_out[AE_CH_SR] >= 0)   m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_SR;
    if (m_idx_out[AE_CH_TFL] >= 0)  m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_TFL;
    if (m_idx_out[AE_CH_TFR] >= 0)  m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_TFR;
    if (m_idx_out[AE_CH_TFC] >= 0)  m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_TFC;
    if (m_idx_out[AE_CH_TC] >= 0)   m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_TC;
    if (m_idx_out[AE_CH_TBL] >= 0)  m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_TBL;
    if (m_idx_out[AE_CH_TBR] >= 0)  m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_TBR;
    if (m_idx_out[AE_CH_TBC] >= 0)  m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_TBC;
    if (m_idx_out[AE_CH_BLOC] >= 0) m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_BLOC;
    if (m_idx_out[AE_CH_BROC] >= 0) m_AddonSettings.lOutChannelPresentFlags |= AE_DSP_PRSNT_CH_BROC;

    ClearArray(m_AudioInArray, m_AudioInArraySize);

    if (m_Addon_InputResample)
      ClearArray(m_InputResampleArray, m_InputResampleArraySize);

    if (m_Addon_MasterProc)
      ClearArray(m_MasterArray, m_MasterArraySize);

    if (!m_Addons_PostProc.empty())
    {
      ClearArray(m_PostProcessArray[0], m_PostProcessArraySize);
      ClearArray(m_PostProcessArray[1], m_PostProcessArraySize);
    }

    if (m_Addon_OutputResample)
      ClearArray(m_OutputResampleArray, m_OutputResampleArraySize);

    m_AddonSettings.iStreamID               = m_StreamId;
    m_AddonSettings.iInChannels             = in->pkt->config.channels;
    m_AddonSettings.iOutChannels            = out->pkt->config.channels;
    m_AddonSettings.iInSamplerate           = in->pkt->config.sample_rate;
    m_AddonSettings.iProcessSamplerate      = m_Addon_InputResample  ? m_Addon_InputResample->InputResampleSampleRate(m_StreamId)   : m_AddonSettings.iInSamplerate;
    m_AddonSettings.iOutSamplerate          = m_Addon_OutputResample ? m_Addon_OutputResample->OutputResampleSampleRate(m_StreamId) : m_AddonSettings.iProcessSamplerate;

    if (m_NewMasterMode >= 0)
    {
      MasterModeChange(m_NewMasterMode, m_NewStreamType);
      m_NewMasterMode = AE_DSP_MASTER_MODE_ID_INVALID;
      m_NewStreamType = AE_DSP_ASTREAM_INVALID;
    }

    for (AE_DSP_ADDONMAP_ITR itr = m_usedMap.begin(); itr != m_usedMap.end(); itr++)
    {
      AE_DSP_ERROR err = itr->second->StreamInitialize(&m_AddonSettings);
      if (err != AE_DSP_ERROR_NO_ERROR)
      {
        CLog::Log(LOGERROR, "ActiveAE DSP - %s - addon initialize failed on %s with %s", __FUNCTION__, itr->second->GetAudioDSPName().c_str(), CActiveAEDSPAddon::ToString(err));
      }
    }

    needDSPAddonsReinit = false;
    m_ForceInit = false;
  }

  float * DataIn          = (float *)in->pkt->data[0];
  int     channelsIn      = in->pkt->config.channels;
  unsigned int framesOut  = in->pkt->nb_samples;
  float * DataOut         = (float *)out->pkt->data[0];
  int     channelsOut     = out->pkt->config.channels;

  /* Check for big enough input array */
  if (framesOut > m_AudioInArraySize)
  {
    m_AudioInArraySize = framesOut + MIN_DSP_ARRAY_SIZE / 10;
    for (int i = 0; i < AE_DSP_CH_MAX; i++)
    {
      m_AudioInArray[i] = (float*)realloc(m_AudioInArray[i], m_AudioInArraySize*sizeof(float));
      if (m_AudioInArray == NULL)
      {
        CLog::Log(LOGERROR, "ActiveAE DSP - %s - realloc for input data array failed", __FUNCTION__);
        return false;
      }
    }
  }

  /* Put every channel from the interleaved buffer to a own buffer */
  ptr = 0;
  for (unsigned int i = 0; i < framesOut; i++)
  {
    m_AudioInArray[AE_DSP_CH_FL][i]   = m_idx_in[AE_CH_FL]   >= 0 ? DataIn[ptr+m_idx_in[AE_CH_FL]]   : 0.0;
    m_AudioInArray[AE_DSP_CH_FR][i]   = m_idx_in[AE_CH_FR]   >= 0 ? DataIn[ptr+m_idx_in[AE_CH_FR]]   : 0.0;
    m_AudioInArray[AE_DSP_CH_FC][i]   = m_idx_in[AE_CH_FC]   >= 0 ? DataIn[ptr+m_idx_in[AE_CH_FC]]   : 0.0;
    m_AudioInArray[AE_DSP_CH_LFE][i]  = m_idx_in[AE_CH_LFE]  >= 0 ? DataIn[ptr+m_idx_in[AE_CH_LFE]]  : 0.0;
    m_AudioInArray[AE_DSP_CH_BR][i]   = m_idx_in[AE_CH_BL]   >= 0 ? DataIn[ptr+m_idx_in[AE_CH_BL]]   : 0.0;
    m_AudioInArray[AE_DSP_CH_BR][i]   = m_idx_in[AE_CH_BR]   >= 0 ? DataIn[ptr+m_idx_in[AE_CH_BR]]   : 0.0;
    m_AudioInArray[AE_DSP_CH_FLOC][i] = m_idx_in[AE_CH_FLOC] >= 0 ? DataIn[ptr+m_idx_in[AE_CH_FLOC]] : 0.0;
    m_AudioInArray[AE_DSP_CH_FROC][i] = m_idx_in[AE_CH_FROC] >= 0 ? DataIn[ptr+m_idx_in[AE_CH_FROC]] : 0.0;
    m_AudioInArray[AE_DSP_CH_BC][i]   = m_idx_in[AE_CH_BC]   >= 0 ? DataIn[ptr+m_idx_in[AE_CH_BC]]   : 0.0;
    m_AudioInArray[AE_DSP_CH_SL][i]   = m_idx_in[AE_CH_SL]   >= 0 ? DataIn[ptr+m_idx_in[AE_CH_SL]]   : 0.0;
    m_AudioInArray[AE_DSP_CH_SR][i]   = m_idx_in[AE_CH_SR]   >= 0 ? DataIn[ptr+m_idx_in[AE_CH_SR]]   : 0.0;
    m_AudioInArray[AE_DSP_CH_TC][i]   = m_idx_in[AE_CH_TC]   >= 0 ? DataIn[ptr+m_idx_in[AE_CH_TC]]   : 0.0;
    m_AudioInArray[AE_DSP_CH_TFL][i]  = m_idx_in[AE_CH_TFL]  >= 0 ? DataIn[ptr+m_idx_in[AE_CH_TFL]]  : 0.0;
    m_AudioInArray[AE_DSP_CH_TFC][i]  = m_idx_in[AE_CH_TFC]  >= 0 ? DataIn[ptr+m_idx_in[AE_CH_TFC]]  : 0.0;
    m_AudioInArray[AE_DSP_CH_TFR][i]  = m_idx_in[AE_CH_TFR]  >= 0 ? DataIn[ptr+m_idx_in[AE_CH_TFR]]  : 0.0;
    m_AudioInArray[AE_DSP_CH_TBL][i]  = m_idx_in[AE_CH_TBL]  >= 0 ? DataIn[ptr+m_idx_in[AE_CH_TBL]]  : 0.0;
    m_AudioInArray[AE_DSP_CH_TBC][i]  = m_idx_in[AE_CH_TBC]  >= 0 ? DataIn[ptr+m_idx_in[AE_CH_TBC]]  : 0.0;
    m_AudioInArray[AE_DSP_CH_TBR][i]  = m_idx_in[AE_CH_TBR]  >= 0 ? DataIn[ptr+m_idx_in[AE_CH_TBR]]  : 0.0;
    m_AudioInArray[AE_DSP_CH_BLOC][i] = m_idx_in[AE_CH_BLOC] >= 0 ? DataIn[ptr+m_idx_in[AE_CH_BLOC]] : 0.0;
    m_AudioInArray[AE_DSP_CH_BROC][i] = m_idx_in[AE_CH_BROC] >= 0 ? DataIn[ptr+m_idx_in[AE_CH_BROC]] : 0.0;

    ptr += channelsIn;
  }

    /**********************************************/
   /** DSP Processing Algorithms following here **/
  /**********************************************/

  float **lastOutArray  = m_AudioInArray;             // If nothing becomes performed copy in to out
  unsigned int frames   = framesOut;                  // frames out is on default the same as in

  /**
   * DSP pre processing
   * Can be used to rework input stream to remove maybe stream faults.
   * Channels (upmix/downmix) or sample rate is not to change! Only the
   * input stream data can be changed, no access to other data point!
   * All DSP addons allowed todo this.
   */
  for (unsigned int i = 0; i < m_Addons_PreProc.size(); i++)
  {
    try
    {
      if (!m_Addons_PreProc[i]->PreProcess(m_StreamId, lastOutArray, frames))
      {
        CLog::Log(LOGERROR, "ActiveAE DSP - %s - pre process failed on addon No. %i", __FUNCTION__, i);
        return false;
      }
    }
    catch (exception &e)
    {
      CLog::Log(LOGERROR, "ActiveAE DSP - exception '%s' caught while trying to call 'PreProcess' on add-on id '%i'. Please contact the developer of this add-on", e.what(), i);
      m_Addons_PreProc.erase(m_Addons_PreProc.begin()+i);
      i--;
    }
  }

  /**
   * DSP resample processing before master
   * Here a high quality resample can be performed.
   * Only one DSP addon is allowed todo this!
   */
  if (m_Addon_InputResample)
  {
    /* Check for big enough array */
    try
    {
      framesOut = m_Addon_InputResample->InputResampleProcessNeededSamplesize(m_StreamId);
      if (framesOut > m_InputResampleArraySize)
      {
        m_InputResampleArraySize = framesOut + MIN_DSP_ARRAY_SIZE / 10;
        for (int i = 0; i < AE_DSP_CH_MAX; i++)
        {
          m_InputResampleArray[i] = (float*)realloc(m_InputResampleArray[i], m_InputResampleArraySize*sizeof(float));
          if (m_InputResampleArray[i] == NULL)
          {
            CLog::Log(LOGERROR, "ActiveAE DSP - %s - realloc for pre resample data array failed", __FUNCTION__);
            return false;
          }
        }
      }

      frames = m_Addon_InputResample->InputResampleProcess(m_StreamId, lastOutArray, m_InputResampleArray, frames);
      if (frames == 0)
        return false;

      lastOutArray = m_InputResampleArray;
    }
    catch (exception &e)
    {
      CLog::Log(LOGERROR, "ActiveAE DSP - exception '%s' caught while trying to call 'InputResampleProcess' on add-on'. Please contact the developer of this add-on", e.what());
      m_Addon_InputResample = NULL;
    }
  }

  /**
   * DSP master processing
   * Here a channel upmix/downmix for stereo surround sound can be performed
   * Only one DSP addon is allowed todo this!
   */
  if (m_Addon_MasterProc)
  {
    /* Check for big enough array */
    try
    {
      framesOut = m_Addon_MasterProc->MasterProcessNeededSamplesize(m_StreamId);
      if (framesOut > m_MasterArraySize)
      {
        m_InputResampleArraySize = framesOut + MIN_DSP_ARRAY_SIZE / 10;
        for (int i = 0; i < AE_DSP_CH_MAX; i++)
        {
          m_MasterArray[i] = (float*)realloc(m_MasterArray[i], m_MasterArraySize*sizeof(float));
          if (m_MasterArray[i] == NULL)
          {
            CLog::Log(LOGERROR, "ActiveAE DSP - %s - realloc for master data array failed", __FUNCTION__);
            return false;
          }
        }
      }

      frames = m_Addon_MasterProc->MasterProcess(m_StreamId, lastOutArray, m_MasterArray, frames);
      if (frames == 0)
        return false;

      lastOutArray = m_MasterArray;
    }
    catch (exception &e)
    {
      CLog::Log(LOGERROR, "ActiveAE DSP - exception '%s' caught while trying to call 'MasterProcess' on add-on'. Please contact the developer of this add-on", e.what());
      m_Addon_MasterProc = NULL;
    }
  }

  /**
   * DSP post processing
   * On the post processing can be things performed with additional channel upmix like 6.1 to 7.1
   * or frequency/volume corrections, speaker distance handling, equalizer... .
   * All DSP addons allowed todo this.
   */
  for (unsigned int i = 0; i < m_Addons_PostProc.size(); i++)
  {
    /* Check for big enough array */
    try
    {
      framesOut = m_Addons_PostProc[i]->PostProcessNeededSamplesize(m_StreamId);
      if (framesOut > m_PostProcessArraySize)
      {
        m_PostProcessArraySize = framesOut + MIN_DSP_ARRAY_SIZE / 10;
        for (int i = 0; i < AE_DSP_CH_MAX; i++)
        {
          m_PostProcessArray[0][i] = (float*)realloc(m_PostProcessArray[0][i], m_PostProcessArraySize*sizeof(float));
          if (m_PostProcessArray[0][i] == NULL)
          {
            CLog::Log(LOGERROR, "ActiveAE DSP - %s - realloc for post process data array 0 failed", __FUNCTION__);
            return false;
          }
          m_PostProcessArray[1][i] = (float*)realloc(m_PostProcessArray[1][i], m_PostProcessArraySize*sizeof(float));
          if (m_PostProcessArray[1][i] == NULL)
          {
            CLog::Log(LOGERROR, "ActiveAE DSP - %s - realloc for post process data array 0 failed", __FUNCTION__);
            return false;
          }
        }
      }

      frames = m_Addons_PostProc[i]->PostProcess(m_StreamId, lastOutArray, m_PostProcessArray[m_PostProcessArrayTogglePtr], frames);
      if (frames == 0)
        return false;

      lastOutArray = m_PostProcessArray[m_PostProcessArrayTogglePtr];
      m_PostProcessArrayTogglePtr ^= 1;
    }
    catch (exception &e)
    {
      CLog::Log(LOGERROR, "ActiveAE DSP - exception '%s' caught while trying to call 'PostProcess' on add-on id '%i'. Please contact the developer of this add-on", e.what(), i);
      m_Addons_PostProc.erase(m_Addons_PostProc.begin()+i);
      i--;
    }
  }

  /**
   * DSP resample processing behind master
   * Here a high quality resample can be performed.
   * Only one DSP addon is allowed todo this!
   */
  if (m_Addon_OutputResample)
  {
    /* Check for big enough array */
    try
    {
      framesOut = m_Addon_OutputResample->OutputResampleProcessNeededSamplesize(m_StreamId);
      if (framesOut > m_OutputResampleArraySize)
      {
        m_OutputResampleArraySize = framesOut + MIN_DSP_ARRAY_SIZE / 10;
        for (int i = 0; i < AE_DSP_CH_MAX; i++)
        {
          m_OutputResampleArray[i] = (float*)realloc(m_OutputResampleArray[i], m_OutputResampleArraySize*sizeof(float));
          if (m_OutputResampleArray[i] == NULL)
          {
            CLog::Log(LOGERROR, "ActiveAE DSP - %s - realloc for post resample data array failed", __FUNCTION__);
            return false;
          }
        }
      }

      frames = m_Addon_OutputResample->OutputResampleProcess(m_StreamId, lastOutArray, m_OutputResampleArray, frames);
      if (frames == 0)
        return false;

      lastOutArray = m_OutputResampleArray;
    }
    catch (exception &e)
    {
      CLog::Log(LOGERROR, "ActiveAE DSP - exception '%s' caught while trying to call 'OutputResampleProcess' on add-on'. Please contact the developer of this add-on", e.what());
      m_Addon_InputResample = NULL;
    }
  }

  /* Put every supported channel now back to the interleaved out buffer */
  ptr = 0;
  for (unsigned int i = 0; i < frames; i++)
  {
    if (m_idx_out[AE_CH_FL] >= 0)   DataOut[ptr+m_idx_out[AE_CH_FL]]   = lastOutArray[AE_DSP_CH_FL][i];
    if (m_idx_out[AE_CH_FR] >= 0)   DataOut[ptr+m_idx_out[AE_CH_FR]]   = lastOutArray[AE_DSP_CH_FR][i];
    if (m_idx_out[AE_CH_FC] >= 0)   DataOut[ptr+m_idx_out[AE_CH_FC]]   = lastOutArray[AE_DSP_CH_FC][i];
    if (m_idx_out[AE_CH_LFE] >= 0)  DataOut[ptr+m_idx_out[AE_CH_LFE]]  = lastOutArray[AE_DSP_CH_LFE][i];
    if (m_idx_out[AE_CH_BL] >= 0)   DataOut[ptr+m_idx_out[AE_CH_BL]]   = lastOutArray[AE_DSP_CH_BL][i];
    if (m_idx_out[AE_CH_BR] >= 0)   DataOut[ptr+m_idx_out[AE_CH_BR]]   = lastOutArray[AE_DSP_CH_BR][i];
    if (m_idx_out[AE_CH_FLOC] >= 0) DataOut[ptr+m_idx_out[AE_CH_FLOC]] = lastOutArray[AE_DSP_CH_FLOC][i];
    if (m_idx_out[AE_CH_FROC] >= 0) DataOut[ptr+m_idx_out[AE_CH_FROC]] = lastOutArray[AE_DSP_CH_FROC][i];
    if (m_idx_out[AE_CH_BC] >= 0)   DataOut[ptr+m_idx_out[AE_CH_BC]]   = lastOutArray[AE_DSP_CH_BC][i];
    if (m_idx_out[AE_CH_SL] >= 0)   DataOut[ptr+m_idx_out[AE_CH_SL]]   = lastOutArray[AE_DSP_CH_SL][i];
    if (m_idx_out[AE_CH_SR] >= 0)   DataOut[ptr+m_idx_out[AE_CH_SR]]   = lastOutArray[AE_DSP_CH_SR][i];
    if (m_idx_out[AE_CH_TC] >= 0)   DataOut[ptr+m_idx_out[AE_CH_TC]]   = lastOutArray[AE_DSP_CH_TC][i];
    if (m_idx_out[AE_CH_TFL] >= 0)  DataOut[ptr+m_idx_out[AE_CH_TFL]]  = lastOutArray[AE_DSP_CH_TFL][i];
    if (m_idx_out[AE_CH_TFC] >= 0)  DataOut[ptr+m_idx_out[AE_CH_TFC]]  = lastOutArray[AE_DSP_CH_TFC][i];
    if (m_idx_out[AE_CH_TFR] >= 0)  DataOut[ptr+m_idx_out[AE_CH_TFR]]  = lastOutArray[AE_DSP_CH_TFR][i];
    if (m_idx_out[AE_CH_TBL] >= 0)  DataOut[ptr+m_idx_out[AE_CH_TBL]]  = lastOutArray[AE_DSP_CH_TBL][i];
    if (m_idx_out[AE_CH_TBC] >= 0)  DataOut[ptr+m_idx_out[AE_CH_TBC]]  = lastOutArray[AE_DSP_CH_TBC][i];
    if (m_idx_out[AE_CH_TBR] >= 0)  DataOut[ptr+m_idx_out[AE_CH_TBR]]  = lastOutArray[AE_DSP_CH_TBR][i];
    if (m_idx_out[AE_CH_BLOC] >= 0) DataOut[ptr+m_idx_out[AE_CH_BLOC]] = lastOutArray[AE_DSP_CH_BLOC][i];
    if (m_idx_out[AE_CH_BROC] >= 0) DataOut[ptr+m_idx_out[AE_CH_BROC]] = lastOutArray[AE_DSP_CH_BROC][i];

    ptr += channelsOut;
  }
  out->pkt->nb_samples = frames;

  return true;
}

float CActiveAEDSPProcess::GetDelay()
{
  float delay = 0;

  if (m_Addon_InputResample)
  {
    try
    {
      delay += m_Addon_InputResample->InputResampleGetDelay(m_StreamId);
    }
    catch (exception &e)
    {
      CLog::Log(LOGERROR, "ActiveAE DSP - exception '%s' caught while trying to call 'InputResampleGetDelay' on add-on'. Please contact the developer of this add-on", e.what());
      m_Addon_InputResample = NULL;
    }
  }

  if (m_Addon_MasterProc)
  {
    try
    {
      delay += m_Addon_MasterProc->MasterProcessGetDelay(m_StreamId);
    }
    catch (exception &e)
    {
      CLog::Log(LOGERROR, "ActiveAE DSP - exception '%s' caught while trying to call 'MasterProcessGetDelay' on add-on'. Please contact the developer of this add-on", e.what());
      m_Addon_MasterProc = NULL;
    }
  }

  for (unsigned int i = 0; i < m_Addons_PostProc.size(); i++)
  {
    try
    {
      delay += m_Addons_PostProc[i]->PostProcessGetDelay(m_StreamId);
    }
    catch (exception &e)
    {
      CLog::Log(LOGERROR, "ActiveAE DSP - exception '%s' caught while trying to call 'PostProcessGetDelay' on add-on id '%i'. Please contact the developer of this add-on", e.what(), i);
      m_Addons_PostProc.erase(m_Addons_PostProc.begin()+i);
      i--;
    }
  }

  if (m_Addon_OutputResample)
  {
    try
    {
      delay += m_Addon_OutputResample->OutputResampleGetDelay(m_StreamId);
    }
    catch (exception &e)
    {
      CLog::Log(LOGERROR, "ActiveAE DSP - exception '%s' caught while trying to call 'OutputResampleGetDelay' on add-on'. Please contact the developer of this add-on", e.what());
      m_Addon_InputResample = NULL;
    }
  }

  return delay;
}

bool CActiveAEDSPProcess::SetMasterMode(AE_DSP_STREAMTYPE streamType, int iModeID, bool bSwitchStreamType)
{
  /*!
   * if the unique master mode id is already used a reinit is not needed
   */
  if (m_ActiveMode >= AE_DSP_MASTER_MODE_ID_PASSOVER && m_MasterModes[m_ActiveMode]->ModeID() == iModeID && !bSwitchStreamType)
    return true;

  CSingleLock lock(m_restartSection);

  m_NewMasterMode = iModeID;
  m_NewStreamType = bSwitchStreamType ? streamType : AE_DSP_ASTREAM_INVALID;
  m_ForceInit     = true;
  return true;
}

int CActiveAEDSPProcess::GetMasterModeID()
{
  return m_ActiveMode < 0 ? AE_DSP_MASTER_MODE_ID_INVALID : m_MasterModes[m_ActiveMode]->ModeID();
}

void CActiveAEDSPProcess::GetAvailableMasterModes(AE_DSP_STREAMTYPE streamType, AE_DSP_MODELIST &modes)
{
  CSingleLock lock(m_critSection);

  for (unsigned int i = 0; i < m_MasterModes.size(); i++)
  {
    if (m_MasterModes[i]->SupportStreamType(streamType))
      modes.push_back(m_MasterModes[i]);
  }
}

CActiveAEDSPModePtr CActiveAEDSPProcess::GetMasterModeByAddon(int iAddonID, int iModeNumber) const
{
  CSingleLock lock(m_critSection);

  CActiveAEDSPModePtr mode;

  for (unsigned int ptr = 0; ptr < m_MasterModes.size(); ptr++)
  {
    mode = m_MasterModes.at(ptr);
    if (mode->AddonID() == iAddonID &&
        mode->AddonModeNumber() == iModeNumber)
      return mode;
  }

  return mode;
}

CActiveAEDSPModePtr CActiveAEDSPProcess::GetMasterModeRunning() const
{
  CSingleLock lock(m_critSection);

  CActiveAEDSPModePtr mode;

  if (!m_Addon_MasterProc || m_ActiveMode < 0)
    return mode;

  mode = m_MasterModes[m_ActiveMode];
  return mode;
}
