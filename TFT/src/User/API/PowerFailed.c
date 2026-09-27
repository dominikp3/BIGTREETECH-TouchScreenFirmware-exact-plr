#include "PowerFailed.h"
#include "includes.h"

#define BREAK_POINT_FILE "Printing.sys"
#define PLR_FILE_MAGIC 0x32524C50UL
#define PLR_FILE_VERSION 3
#define PLR_PENDING_SIZE 20
#define PLR_HISTORY_SIZE 64
#define PLR_REWIND_MOVES 16

typedef struct
{
  uint32_t magic;
  uint16_t version;
  uint16_t breakpoint_size;
} PLR_FILE_HEADER;

typedef struct
{
  float    axis[TOTAL_AXIS];
  uint32_t feedrate;
  uint16_t speed;
  uint16_t flow;
  uint16_t target[MAX_HEATER_COUNT];
  uint16_t fan[MAX_FAN_COUNT];
  uint8_t  tool;
  uint32_t offset;
  bool     relative;
  bool     relative_e;
  bool     pause;
  bool     ups_z_raised;
  float    physical_z;
  bool     physical_z_valid;
  float    physical_e;
  bool     physical_e_valid;
} BREAK_POINT;

static BREAK_POINT infoBreakPoint;

typedef struct
{
  BREAK_POINT breakpoint;
  BREAK_POINT resume;
  uint32_t line_number;
  bool valid;
  bool motion;
} PLR_PENDING;

static PLR_PENDING plrPending[PLR_PENDING_SIZE];

typedef struct
{
  BREAK_POINT resume;
  uint32_t line_number;
  bool motion;
} PLR_HISTORY;

static PLR_HISTORY plrHistory[PLR_HISTORY_SIZE];

static uint8_t plrPendingRead = 0;
static uint8_t plrPendingWrite = 0;
static uint8_t plrPendingCount = 0;
static uint8_t plrHistoryWrite = 0;
static uint8_t plrHistoryCount = 0;

static BREAK_POINT plrPrepared;
static bool plrPreparedValid = false;
static bool plrTrackingReliable = true;

static BREAK_POINT plrPauseBreakpoint;
static bool plrPauseBreakpointValid = false;

static bool infoBreakPointValid = false;

static float lastSavedZ = 0.0f;
static bool lastSavedZValid = false;

static bool powerLossActive = false;

static char powerFailedFileName[sizeof(BREAK_POINT_FILE) + 20 + 1];  // extra 20 chars for concatenation with string returned by getFS()
static FIL fpPowerFailed;

static bool restore = false;     // print restore flag disabled by default
static bool restore_ok = false;  // print restore initialization flag disabled by default
static bool restore_heating = false;  // restore preheat stage was enqueued
static bool load_ok = false;     // PLR file loading flag disabled by default
static bool create_ok = false;   // PLR file creation flag disabled by default

static const PLR_FILE_HEADER plrFileHeader =
{
  PLR_FILE_MAGIC,
  PLR_FILE_VERSION,
  sizeof(BREAK_POINT)
};

static void powerFailedCapture(BREAK_POINT *bp, uint32_t offset);
static bool powerFailedSelectFallback(BREAK_POINT *bp);

void powerFailedAckReset(void)
{
  plrPendingRead = 0;
  plrPendingWrite = 0;
  plrPendingCount = 0;
  plrHistoryWrite = 0;
  plrHistoryCount = 0;
  plrPreparedValid = false;
  plrTrackingReliable = true;
  plrPauseBreakpointValid = false;
}

void powerFailedAckInvalidate(void)
{
  plrTrackingReliable = false;
}

void powerFailedSetRestore(bool allowed)
{
  restore = allowed;
}

bool powerFailedGetRestore(void)
{
  return restore;
}

static void powerFailedSetDriverSource(void)
{
  sprintf(powerFailedFileName, "%s%s", getFS(), BREAK_POINT_FILE);
}

static void powerFailedRemoveFile(void)
{
  if (create_ok)
  {
    f_close(&fpPowerFailed);
    create_ok = false;
  }

  powerFailedSetDriverSource();
  f_unlink(powerFailedFileName);
}

bool powerFailedLoad(FIL * print_fp)
{
  // set status flag first
  load_ok = false;
  restore_ok = false;
  restore_heating = false;

  // if print restore flag is disabled, nothing to do
  if (!restore)
    return false;

  // disable print restore flag (one shot flag) for the next print.
  // The flag must always be explicitly re-enabled (e.g by powerFailedSetRestore function)
  restore = false;

  // try to load PLR info from file in order to restore the print from the failed point

  FIL fp;
  UINT br;
  uint8_t model_icon;
  PLR_FILE_HEADER header;

  powerFailedSetDriverSource();

  if (f_open(&fp, powerFailedFileName, FA_OPEN_EXISTING | FA_READ) != FR_OK)
    return false;

  if (f_lseek(&fp, MAX_PATH_LEN) == FR_OK)
  {
    if (f_read(&fp, &model_icon, 1, &br) == FR_OK && br == 1)
    {
      if (f_read(&fp, &header, sizeof(header), &br) == FR_OK &&
          br == sizeof(header) &&
          header.magic == PLR_FILE_MAGIC &&
          header.version == PLR_FILE_VERSION &&
          header.breakpoint_size == sizeof(BREAK_POINT) &&
          f_read(&fp, &infoBreakPoint, sizeof(infoBreakPoint), &br) == FR_OK &&
          br == sizeof(infoBreakPoint))
      {
        // set offset on print file once infoBreakPoint was successfully read
        if (f_lseek(print_fp, infoBreakPoint.offset) == FR_OK)
        {
          load_ok = true;
          restore_ok = true;

          infoBreakPointValid = true;

          lastSavedZ = infoBreakPoint.axis[Z_AXIS];
          lastSavedZValid = true;

          powerFailedAckReset();
        }
      }
    }
  }

  f_close(&fp);  // always close the file once it was successfully open before

  if (load_ok)  // if the file was successfully open, set icon model
    setPrintModelIcon(model_icon);

  return load_ok;
}

bool powerFailedInitRestore(void)
{
  // if print restore initialization flag is disabled, nothing to do
  if (!restore_ok)
    return false;

  if (!restore_heating)
  {
    if (infoBreakPoint.feedrate != 0)
    {
      uint16_t z_raised = 0;
      const bool upsZRaised = infoBreakPoint.ups_z_raised ||
                              (!infoBreakPoint.physical_z_valid && infoSettings.btt_ups == 1);

      // Prefer the exact post-outage Z reported by Marlin. If that final
      // message didn't reach persistent storage, a configured BTT UPS still
      // means the hardware performed its emergency raise. This fallback is
      // especially important for an already-paused print, where pause and UPS
      // raises are cumulative.
      if (upsZRaised)
        z_raised += infoSettings.plr_z_raise;

      if (infoBreakPoint.pause)
        z_raised += infoSettings.pause_z_raise;

      // A power cycle resets Marlin to absolute positioning, but set it
      // explicitly before moving so recovery never depends on startup state.
      mustStoreCmd("G90\n");
      // G92 may create a workspace offset in Marlin, leaving the native
      // stepper coordinate unchanged. PLR needs the physical/native position
      // so a later outage in the resumed print reports the correct Z again.
      mustStoreCmd("G92.9 Z%.3f\n",
                   infoBreakPoint.physical_z_valid
                     ? infoBreakPoint.physical_z
                     : infoBreakPoint.axis[Z_AXIS] + z_raised);

      if (upsZRaised)
        mustStoreCmd("G1 Z%.3f\n", infoBreakPoint.axis[Z_AXIS] + infoSettings.plr_z_raise);

      if (infoSettings.plr_home)
      {
        mustStoreCmd("G28\n");

        if (upsZRaised)
          mustStoreCmd("G1 Z%.3f\n",
                      infoBreakPoint.axis[Z_AXIS] + infoSettings.plr_z_raise);
      }
      else
      {
        mustStoreCmd("G28 R0 XY\n");
      }

      // Move away from the X endstop after homing. On printers using a HallON
      // probe this releases its deployment button and leaves time to stow the
      // probe safely while the heaters reach their targets.
      mustStoreCmd("G1 X0 F3000\n");
      mustStoreCmd("M400\n");
    }

    mustStoreCmd("%s\n", toolChange[infoBreakPoint.tool]);

    for (uint8_t i = MAX_HEATER_COUNT - 1; i >= MAX_HOTEND_COUNT; i--)  // bed & chamber
    {
      if (infoBreakPoint.target[i] != 0)
        mustStoreCmd("%s S%d\n", heatWaitCmd[i], infoBreakPoint.target[i]);
    }

    for (int8_t i = infoSettings.hotend_count - 1; i >= 0; i--)  // tool nozzle
    {
      if (infoBreakPoint.target[i] != 0)
        mustStoreCmd("%s S%d\n", heatWaitCmd[i], infoBreakPoint.target[i]);
    }

    restore_heating = true;
    return true;
  }

  // Emulated M109/M190 commands are sent to Marlin as M104/M140. They stop
  // feeding commands from the print file, but do not stop commands already in
  // the TFT queue. Keep the recovery moves out of that queue until heating has
  // really finished and every command from the preheat stage was acknowledged.
  if (heatIsWaiting() || !isIdleCmdQueue())
    return true;

  // Disable restore initialization before enqueueing the final stage.
  restore_ok = false;
  restore_heating = false;

  for (uint8_t i = 0; i < infoSettings.fan_count; i++)
  {
    if (infoBreakPoint.fan[i] != 0)
      mustStoreCmd(fanCmd[i], infoBreakPoint.fan[i]);
  }

  if (infoBreakPoint.feedrate != 0)
  {
    mustStoreCmd("M83\n");

    if (infoBreakPoint.physical_e_valid)
    {
      // Return the filament to the checkpoint's exact logical E. This
      // compensates both the TFT pause retract and Marlin's UPS retract, even
      // if power failed part-way through an extrusion or pause move.
      float recoveryE = infoBreakPoint.axis[E_AXIS] - infoBreakPoint.physical_e;
      const float extraPurge = infoSettings.resume_purge_len - infoSettings.pause_retract_len;

      if (extraPurge > 0.0f)
        recoveryE += extraPurge;

      if (recoveryE != 0.0f)
        mustStoreCmd("G1 E%.5f F300\n", recoveryE);
    }
    else
    {
      // Compatibility fallback for a checkpoint without physical E.
      mustStoreCmd("G1 E30 F300\n");
      mustStoreCmd("G1 E-%.5f F4800\n", infoSettings.pause_retract_len);
    }

    mustStoreCmd("G1 X%.3f Y%.3f Z%.3f F3000\n", infoBreakPoint.axis[X_AXIS], infoBreakPoint.axis[Y_AXIS], infoBreakPoint.axis[Z_AXIS]);

    if (!infoBreakPoint.physical_e_valid)
      mustStoreCmd("G1 E%.5f F4800\n", infoSettings.resume_purge_len);

    // Keep these as separate queue entries. With command checksums enabled,
    // putting two newline-separated commands in one entry gives only the
    // second line a checksum and Marlin rejects the first one.
    mustStoreCmd("G92 E%.5f\n", infoBreakPoint.axis[E_AXIS]);
    mustStoreCmd("G1 F%d\n", infoBreakPoint.feedrate);
    mustStoreCmd(infoBreakPoint.relative ? "G91\n" : "G90\n");

    if (infoBreakPoint.relative_e == false)
      mustStoreCmd("M82\n");
  }

  return true;
}

bool powerFailedExist(void)
{
  bool access_ok = false;
  FIL fp;
  UINT br;
  uint8_t model_icon;
  PLR_FILE_HEADER header;
  char storedPath[MAX_PATH_LEN];

  powerFailedSetDriverSource();

  if (f_open(&fp, powerFailedFileName, FA_OPEN_EXISTING | FA_READ) != FR_OK)
    return false;

  // Validate the complete record before changing the active browser path.
  // An old or damaged Printing.sys must not leave infoFile.path pointing at
  // a stale directory when this function returns false.
  if (f_read(&fp, storedPath, MAX_PATH_LEN, &br) == FR_OK &&
      br == MAX_PATH_LEN &&
      f_read(&fp, &model_icon, 1, &br) == FR_OK &&
      br == 1 &&
      f_read(&fp, &header, sizeof(header), &br) == FR_OK &&
      br == sizeof(header) &&
      header.magic == PLR_FILE_MAGIC &&
      header.version == PLR_FILE_VERSION &&
      header.breakpoint_size == sizeof(BREAK_POINT))
  {
    memcpy(infoFile.path, storedPath, MAX_PATH_LEN);
    access_ok = true;
  }

  f_close(&fp);  // always close the file once it was successfully open before

  return access_ok;
}

void powerFailedCreate(const char * path)
{
  // close and delete PLR file, if any, first
  powerFailedRemoveFile();
  powerLossActive = false;

  // if PLR is disabled, nothing to do
  if (!infoSettings.plr)
    return;

  if (infoFile.source >= FS_ONBOARD_MEDIA)  // onboard media not supported now
    return;

  UINT br;

  powerFailedSetDriverSource();

  if (f_open(&fpPowerFailed, powerFailedFileName, FA_OPEN_ALWAYS | FA_WRITE) != FR_OK)
    return;

  if (f_write(&fpPowerFailed, path, MAX_PATH_LEN, &br) == FR_OK)
  {
    uint8_t model_icon = isPrintModelIcon();

    if (f_write(&fpPowerFailed, &model_icon, 1, &br) == FR_OK)
    {
      if (f_write(&fpPowerFailed, &plrFileHeader, sizeof(plrFileHeader), &br) == FR_OK &&
          br == sizeof(plrFileHeader))
      {
        // If the PLR file was not loaded, initialize data. Otherwise preserve
        // it so powerFailedInitRestore() can still use the loaded checkpoint.
        if (!load_ok)
        {
          memset(&infoBreakPoint, 0, sizeof(infoBreakPoint));

          infoBreakPointValid = false;
          lastSavedZValid = false;
        }

        if (f_write(&fpPowerFailed, &infoBreakPoint, sizeof(infoBreakPoint), &br) == FR_OK &&
            br == sizeof(infoBreakPoint))
        {
          if (f_sync(&fpPowerFailed) == FR_OK)
          {
            create_ok = true;
            powerFailedAckReset();
          }
        }
      }
    }
  }

  if (!create_ok)  // if PLR file was not properly opened and written, close it
    f_close(&fpPowerFailed);

  return;
}

static void powerFailedWrite(const BREAK_POINT *bp)
{
  if (!create_ok || bp == NULL)
    return;

  UINT br;

  if (f_lseek(&fpPowerFailed, MAX_PATH_LEN + 1 + sizeof(PLR_FILE_HEADER)) != FR_OK)
    return;

  if (f_write(&fpPowerFailed,
              bp,
              sizeof(*bp),
              &br) != FR_OK ||
      br != sizeof(*bp))
    return;

  f_sync(&fpPowerFailed);
}

// void powerFailedCache(uint32_t offset)
// {
//   // if PLR file not created, nothing to do
//   if (!create_ok)
//     return;

//   if (infoBreakPoint.axis[Z_AXIS] == coordinateGetAxisTarget(Z_AXIS))  // if Z axis not changed
//     return;

//   if (!isPaused())  // if not paused, update printing progress status
//   {
//     infoBreakPoint.offset = offset;

//     for (AXIS i = X_AXIS; i < TOTAL_AXIS; i++)
//     {
//       infoBreakPoint.axis[i] = coordinateGetAxisTarget(i);
//     }

//     infoBreakPoint.feedrate = coordinateGetFeedRate();
//     infoBreakPoint.speed = speedGetCurrentPercent(0);  // speed percent
//     infoBreakPoint.flow = speedGetCurrentPercent(1);   // flow percent

//     for (uint8_t i = 0; i < infoSettings.hotend_count; i++)  // tool nozzle
//     {
//       infoBreakPoint.target[i] = heatGetTargetTemp(i);
//     }

//     for (uint8_t i = MAX_HOTEND_COUNT; i < MAX_HEATER_COUNT; i++)  // bed & chamber
//     {
//       infoBreakPoint.target[i] = heatGetTargetTemp(i);
//     }

//     infoBreakPoint.tool = heatGetToolIndex();

//     for (uint8_t i = 0; i < infoSettings.fan_count; i++)
//     {
//       infoBreakPoint.fan[i] = fanGetCurrentSpeed(i);
//     }

//     infoBreakPoint.relative = coordinateGetRelative();
//     infoBreakPoint.relative_e = coordinateGetRelativeExtruder();
//   }
//   else if (infoBreakPoint.pause)  // if paused and the pause state has been saved, nothing to do
//   {
//     return;
//   }

//   infoBreakPoint.pause = isPaused();
//   infoBreakPoint.ups_z_raised = false;

//   UINT br;

//   f_lseek(&fpPowerFailed, MAX_PATH_LEN + 1);  // infoFile.path + infoPrinting.model_icon
//   f_write(&fpPowerFailed, &infoBreakPoint, sizeof(infoBreakPoint), &br);
//   f_sync(&fpPowerFailed);
// }

void powerFailedSave(void)
{
  if (powerLossActive || !create_ok || !infoBreakPointValid)
    return;

  BREAK_POINT safeBreakpoint = infoBreakPoint;

  // Keep the regular on-media checkpoint behind the complete planner depth
  // too, so recovery remains conservative if the extended action is lost.
  // An exact N/Z emergency checkpoint supersedes this fallback.
  powerFailedSelectFallback(&safeBreakpoint);

  if (lastSavedZValid &&
      safeBreakpoint.axis[Z_AXIS] == lastSavedZ)
    return;

  safeBreakpoint.pause = false;
  safeBreakpoint.ups_z_raised = false;
  safeBreakpoint.physical_z_valid = false;

  powerFailedWrite(&safeBreakpoint);

  lastSavedZ = safeBreakpoint.axis[Z_AXIS];
  lastSavedZValid = true;
}

static bool powerFailedFindLine(uint32_t lineNumber, BREAK_POINT *bp)
{
  for (uint8_t i = 0; i < plrPendingCount; i++)
  {
    PLR_PENDING *entry = &plrPending[(plrPendingRead + i) % PLR_PENDING_SIZE];

    if (entry->valid && entry->line_number == lineNumber)
    {
      *bp = entry->resume;
      return true;
    }
  }

  for (uint8_t i = 0; i < plrHistoryCount; i++)
  {
    uint8_t index = (plrHistoryWrite + PLR_HISTORY_SIZE - 1 - i) % PLR_HISTORY_SIZE;

    if (plrHistory[index].line_number == lineNumber)
    {
      *bp = plrHistory[index].resume;
      return true;
    }
  }

  return false;
}

static bool powerFailedSelectFallback(BREAK_POINT *bp)
{
  uint8_t moves = 0;
  bool found = false;

  for (uint8_t i = 0; i < plrHistoryCount; i++)
  {
    uint8_t index = (plrHistoryWrite + PLR_HISTORY_SIZE - 1 - i) % PLR_HISTORY_SIZE;

    if (!plrHistory[index].motion)
      continue;

    *bp = plrHistory[index].resume;
    found = true;

    if (++moves >= PLR_REWIND_MOVES)
      break;
  }

  return found;
}

void powerFailedEmergencySave(uint32_t lineNumber, bool lineNumberValid,
                              float physicalZ, bool physicalZValid,
                              float physicalE, bool physicalEValid)
{
  if (powerLossActive || !create_ok)
    return;

  BREAK_POINT emergencyBreakpoint;
  bool selected = false;

  // Commands used to retract, raise, and park during a TFT pause don't come
  // from the print file. Once the pause origin has been synchronized with
  // M400, always recover from that exact print position instead of trying to
  // match the transport line number of a parking move.
  if (isPaused() && plrPauseBreakpointValid)
  {
    emergencyBreakpoint = plrPauseBreakpoint;
    selected = true;
  }
  else if (lineNumberValid && plrTrackingReliable)
    selected = powerFailedFindLine(lineNumber, &emergencyBreakpoint);

  if (!selected)
    selected = powerFailedSelectFallback(&emergencyBreakpoint);

  if (selected)
  {
    infoBreakPoint = emergencyBreakpoint;
    infoBreakPointValid = true;
  }

  if (!infoBreakPointValid)
    return;

  // From this point normal PLR checkpoints must never overwrite
  // the emergency checkpoint.
  powerLossActive = true;

  infoBreakPoint.pause = isPaused();
  infoBreakPoint.ups_z_raised = true;
  infoBreakPoint.physical_z = physicalZ;
  infoBreakPoint.physical_z_valid = physicalZValid;
  infoBreakPoint.physical_e = physicalE;
  infoBreakPoint.physical_e_valid = physicalEValid;

  powerFailedWrite(&infoBreakPoint);
}

static void powerFailedCapture(BREAK_POINT *bp, uint32_t offset)
{
  bp->offset = offset;

  for (AXIS i = X_AXIS; i < TOTAL_AXIS; i++)
    bp->axis[i] = coordinateGetAxisTarget(i);

  bp->feedrate = coordinateGetFeedRate();

  bp->speed = speedGetCurrentPercent(0);
  bp->flow  = speedGetCurrentPercent(1);

  for (uint8_t i = 0; i < infoSettings.hotend_count; i++)
    bp->target[i] = heatGetTargetTemp(i);

  for (uint8_t i = MAX_HOTEND_COUNT; i < MAX_HEATER_COUNT; i++)
    bp->target[i] = heatGetTargetTemp(i);

  bp->tool = heatGetToolIndex();

  for (uint8_t i = 0; i < infoSettings.fan_count; i++)
    bp->fan[i] = fanGetCurrentSpeed(i);

  bp->relative   = coordinateGetRelative();
  bp->relative_e = coordinateGetRelativeExtruder();

  bp->pause = false;
  bp->ups_z_raised = false;
  bp->physical_z = 0.0f;
  bp->physical_z_valid = false;
  bp->physical_e = 0.0f;
  bp->physical_e_valid = false;
}

void powerFailedPrepare(uint32_t resumeOffset, bool valid)
{
  plrPreparedValid = valid;

  if (valid)
    powerFailedCapture(&plrPrepared, resumeOffset);
}

void powerFailedTrackSent(uint32_t endOffset, bool valid,
                          uint32_t lineNumber, bool motion)
{
  if (plrPendingCount >= PLR_PENDING_SIZE)
  {
    plrTrackingReliable = false;
    plrPreparedValid = false;
    return;
  }

  PLR_PENDING *entry = &plrPending[plrPendingWrite];

  entry->valid = valid;
  entry->motion = motion;
  entry->line_number = (lineNumber != 0 && lineNumber != 0xFFFFFFFFUL && plrPreparedValid)
                         ? lineNumber
                         : 0;

  if (valid)
  {
    powerFailedCapture(&entry->breakpoint, endOffset);

    if (plrPreparedValid)
      entry->resume = plrPrepared;
    else if (infoBreakPointValid)
      entry->resume = infoBreakPoint;
    else
      entry->resume = entry->breakpoint;
  }

  plrPendingWrite = (plrPendingWrite + 1) % PLR_PENDING_SIZE;
  plrPendingCount++;
  plrPreparedValid = false;
}

void powerFailedAckConfirm(void)
{
  if (plrPendingCount == 0)
    return;

  PLR_PENDING *entry = &plrPending[plrPendingRead];

  if (entry->valid && !powerLossActive && plrTrackingReliable)
  {
    infoBreakPoint = entry->breakpoint;
    infoBreakPointValid = true;

    PLR_HISTORY *history = &plrHistory[plrHistoryWrite];

    history->resume = entry->resume;
    history->line_number = entry->line_number;
    history->motion = entry->motion;

    plrHistoryWrite = (plrHistoryWrite + 1) % PLR_HISTORY_SIZE;

    if (plrHistoryCount < PLR_HISTORY_SIZE)
      plrHistoryCount++;
  }

  plrPendingRead = (plrPendingRead + 1) % PLR_PENDING_SIZE;
  plrPendingCount--;
}

void powerFailedBeginPause(float x, float y, float z, float e,
                           uint32_t feedrate, bool relative, bool relativeE)
{
  if (powerLossActive || !create_ok || !infoBreakPointValid)
    return;

  // M400 has already completed when this is called, so infoBreakPoint is the
  // end of the last fully executed print-file command and these coordinates
  // are the exact position to which a later recovery must return.
  plrPauseBreakpoint = infoBreakPoint;
  plrPauseBreakpoint.axis[X_AXIS] = x;
  plrPauseBreakpoint.axis[Y_AXIS] = y;
  plrPauseBreakpoint.axis[Z_AXIS] = z;
  plrPauseBreakpoint.axis[E_AXIS] = e;
  plrPauseBreakpoint.feedrate = feedrate;
  plrPauseBreakpoint.relative = relative;
  plrPauseBreakpoint.relative_e = relativeE;
  plrPauseBreakpoint.pause = true;
  plrPauseBreakpoint.ups_z_raised = false;
  plrPauseBreakpoint.physical_z_valid = false;
  plrPauseBreakpoint.physical_e_valid = false;
  plrPauseBreakpointValid = true;

  // Persist the paused checkpoint immediately. The emergency action will
  // update it with the measured post-UPS Z if power is actually lost.
  infoBreakPoint = plrPauseBreakpoint;
  powerFailedWrite(&infoBreakPoint);
}

// void powerFailedEmergencyCache(uint32_t offset)
// {
//   if (!create_ok)
//     return;

//   infoBreakPoint.offset = offset;
//   infoBreakPoint.pause = isPaused();
//   infoBreakPoint.ups_z_raised = true;

//   UINT br;

//   f_lseek(&fpPowerFailed, MAX_PATH_LEN + 1);
//   f_write(&fpPowerFailed, &infoBreakPoint, sizeof(infoBreakPoint), &br);
//   f_sync(&fpPowerFailed);
// }

void powerFailedDelete(void)
{
  powerFailedRemoveFile();

  infoBreakPointValid = false;
  lastSavedZValid = false;

  load_ok = false;
  restore_ok = false;
  restore_heating = false;

  powerFailedAckReset();
  powerLossActive = false;
}
