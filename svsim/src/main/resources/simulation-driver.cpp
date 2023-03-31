// SPDX-License-Identifier: Apache-2.0
/**
 * simulation-driver.cpp
 *
 * An executable harness which exposes a simple protocol for controlling a
 * compiled simulation of a SystemVerilog module and testbench. A host program
 * can launch this executable and communicate with it by sending commands to
 * `stdin` and reading messages from `stdout`. Messages and commands have a
 * simple text-based encoding designed to be simple to implement while
 * mainitaining good performance. Each message or command starts with a single
 * character denoting the type, followed by a sequence of arguments and
 * separator characters. Some separator characters are varied to make it easier
 * to detect where encoding errors occur. Arguments are all hexadecimal values,
 * with arbitrary bit width (the protocol will report an error when attempting
 * to write a value which is too wide for the destination port). Some
 * hexadecimal values may be negative based on context. All messages and
 * commands, with the exception of LOG, consist of a single line of text (i.e.
 * have no internal newlines). LOG is special because it returns the log of the
 * running simulation which may have internal newlines. Specific command formats
 * are described below. Messages and commands are pipelined, meaning it is not
 * required (or recommended) to wait until receiving a message in response to a
 * command before sending the subsequent command. This allows for really good
 * performance in cases where messages can be processed out-of-band, and should
 * be considered when designing the host API.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/personality.h>
#include <unistd.h>

#ifdef SVSIM_ENABLE_VERILATOR_SUPPORT
#include "verilated-sources/VsvsimTestbench__Dpi.h"
#define DPI_TASK_RETURN_TYPE int
#define DPI_TASK_RETURN_VALUE 0
#endif
#ifdef SVSIM_ENABLE_VCS_SUPPORT
#include "vc_hdrs.h"
#define DPI_TASK_RETURN_TYPE void
#define DPI_TASK_RETURN_VALUE
#endif

extern "C" {

/// These functions are generated by svsim
extern int port_getter(int id, int *bitWidth, void (**getter)(uint8_t *));
extern int port_setter(int id, int *bitWidth, void (**setter)(const uint8_t *));

/**
 * The functions in the following block can be implemented either by DPI, or in
 * C++. If they are implemented via DPI, these declarations should match the
 * declarations in the generated DPI headers.
 */
#ifdef __cplusplus
extern "C" {
#endif
extern void run_simulation(int timesteps);
extern void simulation_main(int argc, const char **argv);
#ifdef __cplusplus
}
#endif

// Messages are written by this executable to `stdout`
enum {
  // Format: r ready
  // Sent as the first message to indicate the simulation has started
  // successfully (otherwise an error message will be sent instead). Commands
  // can be sent prior to receiving this message.
  MESSAGE_READY = 'r',

  // Format: e <error message>
  // Sent when an error occurs. The simulation will exit after sending this
  // message.
  MESSAGE_ERROR = 'e',

  // Format: k ack
  // Sent in response to a command which does not return a value.
  MESSAGE_ACK = 'k',

  // Format: b <8-digit bit-width> <value>
  // Sent in response to a command which returns a value. For convenience, the
  // value is prefixed with an 8-digit bit width. The value is encoded as a
  // hexadecimal string and can be negative (prefixed with `-`) if this is the
  // response to `GET` command requesting a signed value.
  MESSAGE_BITS = 'b',

  // Format: l <8-digit byte count> <log data, potentially containg newlines>
  // Sent in response to the LOG command. The length of the log is provided
  // since it may contain newlines.
  MESSAGE_LOG = 'l',
};

// Commands are read by this executable from `stdin`
enum {
  // Format: D
  // Signals that the simulation should exit. The sender should wait for the
  // simulation to exit with a status of 0 to signify success, otherwise the
  // simulation may be in the process of completing some in-flight tasks like
  // writing to a waveform file.
  COMMAND_DONE = 'D',

  // Format: L
  // Requests a LOG message
  COMMAND_LOG = 'L',

  // Format: G [s|u] <port id>
  // Gets the value of a port. The value is returned as a BITS message.
  COMMAND_GET_BITS = 'G',

  // Format: S <port id> <value>
  // Sets the value of a port.
  COMMAND_SET_BITS = 'S',

  // Format: R <timesteps>
  // Runs the simulation for the specified number of timesteps. Returns an ACK
  // message when complete.
  COMMAND_RUN = 'R',

  // Format: T <ticking port id> <in-phase value>,<out-of-phase
  // value>-<timesteps>*<max cycles>[ <sentinel port id> <sentinel value>]
  // Runs the simulation for at most the specified number of cycles. A cycle is
  // defined as setting the ticking port to the "in-phase" state, running the
  // simulation for the specified number of timesteps, then setting the ticking
  // port to the "out-of-phase" state and running the simulation for the
  // specified number of timesteps again (so a cycle is actualy 2x the specified
  // number of timesteps).
  // If a sentinel port and value are provided, the simulation will stop early
  // if the sentinel port is set to the specified value.
  COMMAND_TICK = 'T',

  // Format: X [1|0]
  // Enables ("1") or disables ("0") tracing. This command requires tracing to
  // be set up in the backend via `TraceStyle`, which should make sure the
  // proper arguments are passed to the compiler, including the desired
  // SVSIM_ENABLE_*_TRACING define.
  COMMAND_TRACE = 'W',
};

/**
 * Messages and commands are logged to an execution script for potential replay.
 * Messages start at -1 because the "READY" message does not have an associated
 * command
 */
FILE *executionScript = NULL;
int executionScriptMessageCount = 0;
int executionScriptCommandCount = 1;
int executionScriptLimit = -1;

// -- Sending Messages

bool shouldLogMessageToExecutionScript() {
  return executionScript != NULL &&
         (executionScriptLimit == -1 ||
          executionScriptMessageCount < executionScriptLimit);
}

FILE *messageStream = NULL;
static void writeMessageStart(char messageCode) {
  if (shouldLogMessageToExecutionScript()) {
    fprintf(executionScript, "%d< %c ", executionScriptMessageCount,
            messageCode);
  }
  fprintf(messageStream, "%c ", messageCode);
}
#define writeMessageBody(format, args...)                                      \
  {                                                                            \
    fprintf(messageStream, format, ##args);                                    \
    if (shouldLogMessageToExecutionScript()) {                                 \
      fprintf(executionScript, format, ##args);                                \
    }                                                                          \
  }
static void writeMessageEnd() {
  fprintf(messageStream, "\n");
  fflush(messageStream);
  if (shouldLogMessageToExecutionScript()) {
    fprintf(executionScript, "\n");
    fflush(executionScript);
  }
  executionScriptMessageCount += 1;
}

#define writeMessage(messageCode, format, args...)                             \
  {                                                                            \
    writeMessageStart(messageCode);                                            \
    writeMessageBody(format, ##args);                                          \
    writeMessageEnd();                                                         \
  }

// Formatted string must not contain newlines
#define failWithError(format, args...)                                         \
  {                                                                            \
    writeMessage(MESSAGE_ERROR, format, ##args);                               \
    exit(EXIT_FAILURE);                                                        \
  }

static void sendReady() { writeMessage(MESSAGE_READY, "ready"); }

static void sendAck() { writeMessage(MESSAGE_ACK, "ack"); }

// This method may modify the bytes in the buffer
static void sendBits(uint8_t *mutableBytes, int bitCount, bool isSigned) {
  if (bitCount <= 0) {
    failWithError("Cannot send 0-bit value.");
  }
  if (isSigned && bitCount <= 1) {
    failWithError("Cannot send 1-bit signed value.");
  }
  writeMessageStart(MESSAGE_BITS);
  writeMessageBody("%08X ", bitCount) int byteCount = (bitCount + 7) / 8;
  if (isSigned) {
    uint8_t signBitMask;
    switch (bitCount % 8) {
    case 1:
      signBitMask = 0b00000001;
      break;
    case 2:
      signBitMask = 0b00000010;
      break;
    case 3:
      signBitMask = 0b00000100;
      break;
    case 4:
      signBitMask = 0b00001000;
      break;
    case 5:
      signBitMask = 0b00010000;
      break;
    case 6:
      signBitMask = 0b00100000;
      break;
    case 7:
      signBitMask = 0b01000000;
      break;
    case 0:
      signBitMask = 0b10000000;
      break;
    }
    if (mutableBytes[byteCount - 1] & signBitMask) {
      writeMessageBody("-");
      /// Convert to two's complement
      int carry = 1;
      for (int i = 0; i < byteCount; i++) {
        int byte = mutableBytes[i];
        byte = (~byte & 0xFF) + carry;
        carry = byte >> 8;
        mutableBytes[i] = (uint8_t)byte;
      }
    }
    // Strip irrelevant bits
    mutableBytes[byteCount - 1] &= signBitMask - 1;
  }
  for (int i = byteCount - 1; i >= 0; i--) {
    writeMessageBody("%02X", mutableBytes[i]);
  }
  writeMessageEnd();
}

static void sendUintAsBits(uint64_t value) {
  sendBits((uint8_t *)&value, sizeof(uint64_t) * 8, false);
}

// `logFilePath` is set in `main`
const char *logFilePath = NULL;
static void sendLog() {
  /// `stdout` is a file and needs to be flushed so that the log is present
  fflush(stdout);

  /// RUN responds with a LOG message and the currently logged data
  static FILE *log = NULL;
  if (log == NULL) {
    if (logFilePath == NULL) {
      failWithError("No log file specified.");
    }
    log = fopen(logFilePath, "rb");
    if (log == NULL) {
      failWithError("Could not open log file '%s'.", logFilePath);
    }
  }
  // Determine how many bytes can be read
  fpos_t currentPosition;
  if (fgetpos(log, &currentPosition) != 0) {
    failWithError("Failed to gather log (%d).", __LINE__);
  }
  long currentOffset = ftell(log);
  if (fseek(log, 0, SEEK_END) != 0) {
    failWithError("Failed to gather log (%d).", __LINE__);
  }
  long endOffset = ftell(log);
  if (fsetpos(log, &currentPosition) != 0 || endOffset < currentOffset) {
    failWithError("Failed to gather log (%d).", __LINE__);
  }
  long readByteCount = endOffset - currentOffset;
  if (readByteCount > UINT32_MAX) {
    failWithError("Log is too long to be encoded as a single `LOG` message.");
  }
  // Read available bytes
  char *data = (char *)malloc(readByteCount + 1);
  assert(data != NULL);
  if (fread(data, 1, readByteCount, log) != readByteCount) {
    failWithError("Read an unexpected number of bytes from log.");
  }
  data[readByteCount] = '\0';
  // Send message
  writeMessage(MESSAGE_LOG, "%08lX %s", readByteCount, data);
}

// -- Reading Commands

// A subsequent call to `readCommand` will invalidate the string returned by the
// previous call.
FILE *commandStream = NULL;
static void readCommand(const char **start, const char **end) {
  static char *stringBuffer = NULL;
  static size_t stringBufferLength = 0;
  int byteCount = getline(&stringBuffer, &stringBufferLength, commandStream);
  if (executionScript != NULL) {
    if (executionScriptLimit == -1 ||
        executionScriptCommandCount <= executionScriptLimit) {
      fprintf(executionScript, "%d> %s", executionScriptCommandCount,
              stringBuffer);
      executionScriptCommandCount += 1;
    }
    if (executionScriptCommandCount == executionScriptLimit + 1) {
      fprintf(executionScript,
              "# Execution script limited to %d commands (not counting "
              "implicit 'Done').\n",
              executionScriptLimit);
      fprintf(executionScript, "%d> D\n", executionScriptCommandCount);
    }
    fflush(executionScript);
  }
  const char *stringEnd = stringBuffer + byteCount - 1;
  if (byteCount <= 0) {
    if (feof(stdin)) {
      failWithError("Unexpected end of input.");
    } else {
      failWithError("Could not read next command.");
    }
  } else if (*stringEnd == '\n') {
    *start = stringBuffer;
    *end = stringEnd;
  } else {
    failWithError("Read partial line %s", stringBuffer);
  }
}

/**
 * Scans an integer from the given string, advancing the cursor to the end of
 * the scanned integer.
 * @param description A description of the context in which the integer is being
 * scanned. This is used in error messages and must not contain a newline.
 */
static int scanInt(const char **lineCursor, const char *description) {
  char *scanEnd;
  long value = strtol(*lineCursor, &scanEnd, 16);
  if (scanEnd == *lineCursor) {
    failWithError("Could not scan integer while %s.", description);
  }
  if (value < INT_MIN || value > INT_MAX) {
    failWithError("Scanned out-of-bounds integer while %s.", description);
  }
  *lineCursor = scanEnd;
  return (int)value;
}

int scanHexCharacterReverse(const char **reverseScanCursor,
                            const char *description) {
  char value = **reverseScanCursor;
  if (value >= '0' && value <= '9') {
    (*reverseScanCursor)--;
    return value - '0';
  } else if (value >= 'A' && value <= 'F') {
    (*reverseScanCursor)--;
    return value - 'A' + 10;
  } else if (value >= 'a' && value <= 'f') {
    (*reverseScanCursor)--;
    return value - 'a' + 10;
  } else {
    failWithError("Encountered unexpected character '%c' when %s.", value,
                  description);
  }
}

int scanHexByteReverse(const char **reverseScanCursor,
                       const char *firstCharacterOfValue,
                       const char *description) {
  char low = scanHexCharacterReverse(reverseScanCursor, description);
  if (*reverseScanCursor < firstCharacterOfValue) {
    return low;
  }
  char high = scanHexCharacterReverse(reverseScanCursor, description);
  return (high << 4) | low;
}

/**
 * Returned value must be manually freed.
 */
static uint8_t *scanHexBits(const char **scanCursor, const char *scanEnd,
                            int bitCount, const char *description) {
  const char *reverseScanCursor = scanEnd - 1;
  if (reverseScanCursor < *scanCursor) {
    failWithError("Scanned value is empty when %s.", description);
  }
  if (bitCount <= 0) {
    failWithError("Cannot scan 0-bit-wide value when %s.", description);
  }

  bool isNegative;
  int valueBitCount;
  if (**scanCursor == '-') {
    (*scanCursor)++;
    if (reverseScanCursor < *scanCursor) {
      failWithError("Unexpected end of negative value when %s.", description);
    }
    isNegative = true;
    if (bitCount <= 1) {
      failWithError("Cannot scan 1-bit-wide negative value when %s.",
                    description);
    }
    valueBitCount = bitCount - 1;
  } else {
    isNegative = false;
    valueBitCount = bitCount;
  }

  int byteCount = (bitCount + 7) / 8;
  uint8_t *bytes = (uint8_t *)calloc(sizeof(uint8_t), byteCount);
  assert(bytes != NULL);

  const char *firstCharacterOfValue = *scanCursor;
  int carry = 1; // Only used when `isNegative` is true
  int scannedByteCount = 0;
  int valueByteCount = (valueBitCount + 7) / 8;
  while (scannedByteCount < valueByteCount) {
    int scannedByte = scanHexByteReverse(&reverseScanCursor,
                                         firstCharacterOfValue, description);
    if (isNegative) {
      scannedByte = (~scannedByte & 0xFF) + carry;
      carry = scannedByte >> 8;
      if ((carry & ~1) != 0) {
        failWithError("Unexpected error in carry computation for "
                      "negative value when %s", description);
      }
    }
    bytes[scannedByteCount] = (uint8_t)scannedByte;
    scannedByteCount += 1;
    if (reverseScanCursor < firstCharacterOfValue) {
      break;
    }
  }
  if (reverseScanCursor > firstCharacterOfValue) {
    failWithError("Scanned value exceeded %d bytes when %s.", byteCount,
                  description);
  }

  // A mask of the "inapplicable" bits in the high order byte, used to determine
  // if we received too many bits for the value we are trying to scan. This
  // value could be calculated with bitwise operations, but I find a table to be
  // cleaner and easier to understand. We use `valueBitCount` instead of
  // `bitCount` because the sign bit should be `1` for negative numbers along
  // with all of the other leading bits.
  uint8_t highOrderByteMask;
  switch (valueBitCount % 8) {
  case 1:
    highOrderByteMask = 0b11111110;
    break;
  case 2:
    highOrderByteMask = 0b11111100;
    break;
  case 3:
    highOrderByteMask = 0b11111000;
    break;
  case 4:
    highOrderByteMask = 0b11110000;
    break;
  case 5:
    highOrderByteMask = 0b11100000;
    break;
  case 6:
    highOrderByteMask = 0b11000000;
    break;
  case 7:
    highOrderByteMask = 0b10000000;
    break;
  case 0:
    highOrderByteMask = 0b00000000;
    break;
  }
  if (isNegative) {
    // Ensure we didn't overflow the last byte
    if (carry != 0) {
      failWithError("Scanned negative value exceeded %d bytes when %s.",
                    byteCount, description);
    }
    while (scannedByteCount < byteCount) {
      bytes[scannedByteCount++] = 0xFF;
    }
    // Ensure we didn't overflow inside of the last byte
    if ((bytes[byteCount - 1] & highOrderByteMask) != highOrderByteMask) {
      failWithError("Scanned negative value exceeded %d bits when %s.",
                    bitCount, description);
    }
  } else {
    // Ensure we didn't overflow inside of the last byte
    if ((bytes[byteCount - 1] & highOrderByteMask) != 0) {
      failWithError("Scanned value exceeded %d bits when %s.", bitCount,
                    description);
    }
  }

  *scanCursor = scanEnd;
  return bytes;
}

static const char *findNext(const char *string, char character) {
  const char *cursor = string;
  while (*cursor != '\0' && *cursor != character) {
    cursor++;
  }
  return cursor;
}

// -- Reading and Writing to Ports

typedef struct {
  int bitWidth;
  void (*setter)(const uint8_t *);
} SettablePort;

typedef struct {
  int bitWidth;
  void (*getter)(uint8_t *);
} GettablePort;

static void resolveSettablePort(int id, SettablePort *out,
                                const char *description) {
  out->bitWidth = 0;
  out->setter = NULL;
  if (port_setter(id, &out->bitWidth, &out->setter)) {
    failWithError("Invalid port ID '%d'.", id);
  }
  if (out->bitWidth <= 0) {
    failWithError("Encountered port with invalid bit width when %s.",
                  description);
  }
}

static void resolveGettablePort(int id, GettablePort *out,
                                const char *description) {
  out->bitWidth = 0;
  out->getter = NULL;
  if (port_getter(id, &out->bitWidth, &out->getter)) {
    failWithError("Invalid port ID '%d'.", id);
  }
  if (out->bitWidth <= 0) {
    failWithError("Encountered port with invalid bit width when %s.",
                  description);
  }
}

// -- Processing Commands

const char *simulationTraceFilepath = NULL;

bool receivedDone = false;
static void processCommand() {
  const char *lineCursor = NULL;
  const char *lineEnd = NULL;
  readCommand(&lineCursor, &lineEnd);

  char commandCode = *(lineCursor++);
  switch (commandCode) {
  case COMMAND_DONE: {
    receivedDone = true;
    break;
  }
  case COMMAND_LOG: {
    sendLog();
    break;
  }
  case COMMAND_SET_BITS: {
    uint32_t id = scanInt(&lineCursor, "parsing port ID for SET_BITS command");

    SettablePort port;
    resolveSettablePort(id, &port, "resolving port for SET_BITS command");

    if (*(lineCursor++) != ' ') {
      failWithError("Expected space after port ID for SET_BITS command.");
    }

    const char *valueStart = lineCursor;
    uint8_t *data = scanHexBits(&valueStart, lineEnd, port.bitWidth,
                                "parsing value for SET_BITS command");
    (*port.setter)(data);
    free(data);

    sendAck();
    break;
  }
  case COMMAND_GET_BITS: {
    if (*(lineCursor++) != ' ') {
      failWithError("Expected space after `GET_BITS` command.");
    }

    bool isSigned;
    char foo = *(lineCursor++);
    switch (foo) {
    case 's':
      isSigned = true;
      break;
    case 'u':
      isSigned = false;
      break;
    default:
      failWithError("Expected `s` or `u` argument to `GET_BITS` command (%c).",
                    foo);
    }

    if (*(lineCursor++) != ' ') {
      failWithError(
          "Expected space after `s` or `u` argument to `GET_BITS` command.");
    }

    uint32_t id = scanInt(&lineCursor, "parsing port ID for GET_BITS command");
    if (*lineCursor != '\n') {
      failWithError("Unexpected data at end of GET_BITS command");
    }

    GettablePort port;
    resolveGettablePort(id, &port, "resolving port for GET_BITS command");

    int byteCount = (port.bitWidth + 7) / 8;
    uint8_t *bytes = (uint8_t *)calloc(sizeof(uint8_t), byteCount);
    assert(bytes != NULL);
    (*port.getter)(bytes);
    sendBits(bytes, port.bitWidth, isSigned);
    free(bytes);
    break;
  }
  case COMMAND_RUN: {
    int time = scanInt(&lineCursor, "parsing time for RUN command");
    if (*lineCursor != '\n') {
      failWithError("Unexpected data at end of RUN command.");
    }
    run_simulation(time);

    sendAck();
    break;
  }
  case COMMAND_TICK: {
    // T <ticking-port-ID>
    // <in-phase-value>,<out-of-phase-value>-<timesteps-per-phase>*<max-cycle-count>[
    // <sentinel-port-ID>=<sentinel-value>]

    uint32_t tickingPortID =
        scanInt(&lineCursor, "parsing ticking port ID for TICK command");
    SettablePort tickingPort;
    resolveSettablePort(tickingPortID, &tickingPort,
                        "resolving ticking port for TICK command");

    if (*(lineCursor++) != ' ') {
      failWithError("Expected space after ticking port ID for TICK command.");
    }

    uint8_t *inPhaseValue = scanHexBits(
        &lineCursor, findNext(lineCursor, ','), tickingPort.bitWidth,
        "parsing in-phase value for TICK command");
    if (*(lineCursor++) != ',') {
      failWithError("Expected comma after in-phase value for TICK command.");
    }
    uint8_t *outOfPhaseValue = scanHexBits(
        &lineCursor, findNext(lineCursor, '-'), tickingPort.bitWidth,
        "parsing out-of-phase value for TICK command");
    if (*(lineCursor++) != '-') {
      failWithError("Expected dash after out-of-phase value for TICK command.");
    }

    int timestepsPerPhase =
        scanInt(&lineCursor, "parsing timesteps-per-phase for TICK command");
    if (*(lineCursor++) != '*') {
      failWithError(
          "Expected asterisk after timesteps-per-phase for TICK command.");
    }
    int maxCycleCount =
        scanInt(&lineCursor, "parsing max cycle count for TICK command.");
    if (maxCycleCount <= 0) {
      failWithError(
          "Max cycle count for TICK command should be greater than 0.");
    }

    GettablePort sentinelPort;
    sentinelPort.getter = NULL;
    uint8_t *sentinelValue = NULL;
    uint8_t *sentinelPortValue = NULL;
    int sentinelPortByteCount = 0;
    if (*lineCursor == ' ') {
      lineCursor++;
      uint32_t sentinelPortID =
          scanInt(&lineCursor, "parsing sentinel port ID for TICK command");
      resolveGettablePort(sentinelPortID, &sentinelPort,
                          "resolving sentinel port for TICK command");
      if (*(lineCursor++) != '=') {
        failWithError(
            "Expected equals sign after sentinel port ID for TICK command.");
      }
      sentinelValue = scanHexBits(&lineCursor, lineEnd, sentinelPort.bitWidth,
                                  "parsing sentinel value for TICK command");

      sentinelPortByteCount = (sentinelPort.bitWidth + 7) / 8;
      sentinelPortValue =
          (uint8_t *)calloc(sizeof(uint8_t), sentinelPortByteCount);
      assert(sentinelPortValue != NULL);
    }

    if (*lineCursor != '\n') {
      failWithError("Unexpected data at end of TICK command: %s.", lineCursor);
    }

    int cycles = 0;
    while (cycles++ < maxCycleCount) {
      if (sentinelPort.getter != NULL) {
        (*sentinelPort.getter)(sentinelPortValue);
        if (memcmp(sentinelPortValue, sentinelValue, sentinelPort.bitWidth) ==
            0) {
          break;
        }
      }

      (*tickingPort.setter)(inPhaseValue);
      run_simulation(timestepsPerPhase);
      (*tickingPort.setter)(outOfPhaseValue);
      run_simulation(timestepsPerPhase);
    }

    cycles--; // Consume the unbalanced increment from the while condition
    sendUintAsBits(cycles);

    free(inPhaseValue);
    free(outOfPhaseValue);
    if (sentinelValue != NULL)
      free(sentinelValue);
    if (sentinelPortValue != NULL)
      free(sentinelPortValue);
    break;
  }
  case COMMAND_TRACE: {
    static bool traceInitialized = false;

    if (*(lineCursor++) != ' ') {
      failWithError("Expected space after ticking port ID for TICK command.");
    }

    char argument = *(lineCursor++);

    if (*lineCursor != '\n') {
      failWithError("Unexpected data at end of TRACE command.");
    }

    switch (argument) {
    case '1':
      if (!traceInitialized) {
        traceInitialized = true;
        simulation_initializeTrace(simulationTraceFilepath);
      }
      simulation_enableTrace();
      break;
    case '0':
      simulation_disableTrace();
      break;
    }

    sendAck();
    break;
  }
  default:
    failWithError("Unknown opcode '%d'.", commandCode);
  }
}

bool aslrShenanigansDetected = false;
DPI_TASK_RETURN_TYPE simulation_body() {
  if (aslrShenanigansDetected) {
    failWithError("Backend did not relaunch the executable with ASLR disabled "
                  "as expected.");
  }
  /// If we have made it to `simulation_body`, there were no errors on startup
  /// and the first thing we do is send a READY message.
  sendReady();
  while (!receivedDone)
    processCommand();
  return DPI_TASK_RETURN_VALUE;
}

int main(int argc, const char *argv[]) {
#ifdef SVSIM_BACKEND_ENGAGES_IN_ASLR_SHENANIGANS
  if (!(personality(0xffffffff) & ADDR_NO_RANDOMIZE)) {
    // See note in `Workspace.scala` on
    // SVSIM_BACKEND_ENGAGES_IN_ASLR_SHENANIGANS
    aslrShenanigansDetected = true;
    simulation_main(argc, argv);
    failWithError("simulation_main returned.");
  }
#endif

  // Remap `stdin` and `stdout` so we can use the original `stdin` and `stdout`
  // for commands and messages.
  int stdinCopy = dup(STDIN_FILENO);
  if (stdinCopy == -1) {
    failWithError("Failed to duplicate stdin.");
  }
  commandStream = fdopen(stdinCopy, "r");
  if (commandStream == NULL) {
    failWithError("Failed to open command stream for writing.");
  }
  int stdoutCopy = dup(STDOUT_FILENO);
  if (stdoutCopy == -1) {
    failWithError("Failed to duplicate stdout.");
  }
  messageStream = fdopen(stdoutCopy, "w");
  if (messageStream == NULL) {
    failWithError("Failed to open message stream for reading.");
  }
  if (freopen("/dev/null", "r", stdin) == NULL) {
    failWithError("Failed to redirect stdin to /dev/null.");
  }
  logFilePath = getenv("SVSIM_SIMULATION_LOG");
  if (logFilePath == NULL) {
    logFilePath = "simulation-log.txt";
  }
  if (freopen(logFilePath, "w", stdout) == NULL) {
    failWithError("Failed to redirect stdout to %s.", logFilePath);
  }

  simulationTraceFilepath = getenv("SVSIM_SIMULATION_TRACE");
  if (simulationTraceFilepath == NULL) {
    simulationTraceFilepath = "trace";
  }

  const char *executionScriptLimitString =
      getenv("SVSIM_EXECUTION_SCRIPT_LIMIT");
  if (executionScriptLimitString != NULL) {
    // Adujist limit
    long value = strtol(executionScriptLimitString, NULL, 10);
    if (value < 0 || value > INT_MAX) {
      failWithError("Invalid execution script limit '%ld'.", value);
    }
    executionScriptLimit = (int)value;
  }
  const char *executionScriptPath = getenv("SVSIM_EXECUTION_SCRIPT");
  if (executionScriptPath != NULL) {
    executionScript = fopen(executionScriptPath, "w");
    if (executionScript == NULL) {
      failWithError("Failed to open execution script for writing.");
    }
  }

  simulation_main(argc, argv); // Calls `simulation_body` via DPI
                               /**
                                VCS's implementation of `simulation_main` never returns, so for consistency
                                we should not depend on code running after `simulation_main` regardless of
                                which backend we are using.
                                */
  return 0;
}

} // extern "C"

// -- Verilator Support 

#ifdef SVSIM_ENABLE_VERILATOR_SUPPORT
#include "VsvsimTestbench.h"

extern "C" {

static VerilatedContext *context;
static VsvsimTestbench *testbench;

void simulation_main(int argc, char const **argv) {
  context = new VerilatedContext;
  context->debug(0);

#ifdef SVSIM_VERILATOR_TRACE_ENABLED
  context->traceEverOn(true);
#endif

  context->commandArgs(argc, argv);
  testbench = new VsvsimTestbench{context};

  // Evaluate initial state which should call `simulation_body` via DPI and
  // start the command loop.
  testbench->eval();

  testbench->final();

  delete testbench;
  delete context;
}

void run_simulation(int delay) {
  testbench->eval();
  context->timeInc(delay);
}

} // extern "C"

#endif // SVSIM_ENABLE_VERILATOR_SUPPORT

