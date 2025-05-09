//
//  TTSDKJSONCodec.c
//
//  Created by Karl Stenerud on 2012-01-07.
//
//  Copyright (c) 2012 Karl Stenerud. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall remain in place
// in this source code.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "TTSDKJSONCodec.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// ============================================================================
#pragma mark - Configuration -
// ============================================================================

/** Set to 1 if you're also compiling TTSDKLogger and want to use it here */
#ifndef TTSDKJSONCODEC_UseTTSDKLogger
#define TTSDKJSONCODEC_UseTTSDKLogger 1
#endif

#if TTSDKJSONCODEC_UseTTSDKLogger
#include "TTSDKLogger.h"
#else
#define TTSDKLOG_DEBUG(FMT, ...)
#endif

/** The work buffer size to use when escaping string values.
 * There's little reason to change this since nothing ever gets truncated.
 */
#ifndef TTSDKJSONCODEC_WorkBufferSize
#define TTSDKJSONCODEC_WorkBufferSize 512
#endif

// ============================================================================
#pragma mark - Helpers -
// ============================================================================

// Compiler hints for "if" statements
#define likely_if(x) if (__builtin_expect(x, 1))
#define unlikely_if(x) if (__builtin_expect(x, 0))

/** Used for writing hex string values. */
static char g_hexNybbles[] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F' };

const char *ttsdkjson_stringForError(const int error)
{
    switch (error) {
        case TTSDKJSON_ERROR_INVALID_CHARACTER:
            return "Invalid character";
        case TTSDKJSON_ERROR_DATA_TOO_LONG:
            return "Data too long";
        case TTSDKJSON_ERROR_CANNOT_ADD_DATA:
            return "Cannot add data";
        case TTSDKJSON_ERROR_INCOMPLETE:
            return "Incomplete data";
        case TTSDKJSON_ERROR_INVALID_DATA:
            return "Invalid data";
        default:
            return "(unknown error)";
    }
}

// ============================================================================
#pragma mark - Encode -
// ============================================================================

/** Add JSON encoded data to an external handler.
 * The external handler will decide how to handle the data (store/transmit/etc).
 *
 * @param context The encoding context.
 *
 * @param data The encoded data.
 *
 * @param length The length of the data.
 *
 * @return TTSDKJSON_OK if the data was handled successfully.
 */
#define addJSONData(CONTEXT, DATA, LENGTH) (CONTEXT)->addJSONData(DATA, LENGTH, (CONTEXT)->userData)

/** Escape a string portion for use with JSON and send to data handler.
 *
 * @param context The JSON context.
 *
 * @param string The string to escape and write.
 *
 * @param length The length of the string.
 *
 * @return TTSDKJSON_OK if the data was handled successfully.
 */
static int appendEscapedString(TTSDKJSONEncodeContext *const context, const char *restrict const string, int length)
{
    char workBuffer[TTSDKJSONCODEC_WorkBufferSize];
    const char *const srcEnd = string + length;

    const char *restrict src = string;
    char *restrict dst = workBuffer;

    // Simple case (no escape or special characters)
    for (; src < srcEnd && *src != '\\' && *src != '\"' && (unsigned char)*src >= ' '; src++) {
        *dst++ = *src;
    }

    // Deal with complicated case (if any)
    for (; src < srcEnd; src++) {
        switch (*src) {
            case '\\':
            case '\"':
                *dst++ = '\\';
                *dst++ = *src;
                break;
            case '\b':
                *dst++ = '\\';
                *dst++ = 'b';
                break;
            case '\f':
                *dst++ = '\\';
                *dst++ = 'f';
                break;
            case '\n':
                *dst++ = '\\';
                *dst++ = 'n';
                break;
            case '\r':
                *dst++ = '\\';
                *dst++ = 'r';
                break;
            case '\t':
                *dst++ = '\\';
                *dst++ = 't';
                break;
            default:
                unlikely_if((unsigned char)*src < ' ')
                {
                    TTSDKLOG_DEBUG("Invalid character 0x%02x in string: %s", *src, string);
                    return TTSDKJSON_ERROR_INVALID_CHARACTER;
                }
                *dst++ = *src;
        }
    }
    int encLength = (int)(dst - workBuffer);
    dst -= encLength;
    return addJSONData(context, dst, encLength);
}

/** Escape a string for use with JSON and send to data handler.
 *
 * @param context The JSON context.
 *
 * @param string The string to escape and write.
 *
 * @param length The length of the string.
 *
 * @return TTSDKJSON_OK if the data was handled successfully.
 */
static int addEscapedString(TTSDKJSONEncodeContext *const context, const char *restrict const string, int length)
{
    int result = TTSDKJSON_OK;

    // Keep adding portions until the whole string has been processed.
    int offset = 0;
    while (offset < length) {
        int toAdd = length - offset;
        unlikely_if(toAdd > TTSDKJSONCODEC_WorkBufferSize / 2) { toAdd = TTSDKJSONCODEC_WorkBufferSize / 2; }
        result = appendEscapedString(context, string + offset, toAdd);
        unlikely_if(result != TTSDKJSON_OK) { break; }
        offset += toAdd;
    }
    return result;
}

/** Escape and quote a string for use with JSON and send to data handler.
 *
 * @param context The JSON context.
 *
 * @param string The string to escape and write.
 *
 * @param length The length of the string.
 *
 * @return TTSDKJSON_OK if the data was handled successfully.
 */
static int addQuotedEscapedString(TTSDKJSONEncodeContext *const context, const char *restrict const string, int length)
{
    int result;
    unlikely_if((result = addJSONData(context, "\"", 1)) != TTSDKJSON_OK) { return result; }
    result = addEscapedString(context, string, length);

    // Always close string, even if we failed to write its content
    int closeResult = addJSONData(context, "\"", 1);

    return result || closeResult;
}

/** Check the result of snprintf and handle error cases.
 *
 * @param written The return value from snprintf.
 * @param buffSize The size of the buffer passed to snprintf.
 * @param bytesWritten Pointer to store the number of bytes written.
 * @return TTSDKJSON_OK if successful, or an error code.
 */
static int checkWriteResult(int written, size_t buffSize, int *bytesWritten)
{
    if (written < 0) {
        // An encoding error occurred
        return TTSDKJSON_ERROR_INVALID_CHARACTER;
    } else if (written >= (int)buffSize) {
        // The number was too long to fit in the buffer
        // Note: In this case, buffer is still null-terminated, but truncated
        return TTSDKJSON_ERROR_DATA_TOO_LONG;
    }
    *bytesWritten = written;
    return TTSDKJSON_OK;
}

/** Format a double value to a string buffer.
 *
 * @param buff The buffer to write to.
 * @param buffSize The size of the buffer.
 * @param value The double value to format.
 * @param bytesWritten Pointer to store the number of bytes written.
 * @return TTSDKJSON_OK if successful, or an error code.
 */
static int formatDouble(char *buff, size_t buffSize, double value, int *bytesWritten)
{
    int written = 0;
    if (isnan(value)) {
        written = snprintf(buff, buffSize, "null");
    } else if (isinf(value)) {
        written = snprintf(buff, buffSize, value > 0 ? "1e999" : "-1e999");
    } else {
        float floatValue = (float)value;
        if (fabs(value - floatValue) <= FLT_EPSILON * fabs(value)) {
            written = snprintf(buff, buffSize, "%.*g", FLT_DIG, floatValue);
        } else {
            written = snprintf(buff, buffSize, "%.*g", DBL_DIG, value);
        }

        if (written > 0 && written < (int)buffSize) {
            char *dot = strchr(buff, '.');
            char *e = strchr(buff, 'e');
            if (dot == NULL && e == NULL) {
                written = snprintf(buff, buffSize, "%.1f", value);
            } else if (dot != NULL && e == NULL) {
                char *end = buff + written - 1;
                while (end > dot && *end == '0') {
                    *end-- = '\0';
                    written--;
                }
                if (end == dot) {
                    *++end = '0';
                    written++;
                }
            }
        }
    }
    return checkWriteResult(written, buffSize, bytesWritten);
}

/** Format an int64_t value to a string buffer.
 *
 * @param buff The buffer to write to.
 * @param buffSize The size of the buffer.
 * @param value The int64_t value to format.
 * @param bytesWritten Pointer to store the number of bytes written.
 * @return TTSDKJSON_OK if successful, or an error code.
 */
static int formatInt64(char *buff, size_t buffSize, int64_t value, int *bytesWritten)
{
    int written = snprintf(buff, buffSize, "%" PRId64, value);
    return checkWriteResult(written, buffSize, bytesWritten);
}

/** Format a uint64_t value to a string buffer.
 *
 * @param buff The buffer to write to.
 * @param buffSize The size of the buffer.
 * @param value The uint64_t value to format.
 * @param bytesWritten Pointer to store the number of bytes written.
 * @return TTSDKJSON_OK if successful, or an error code.
 */
static int formatUint64(char *buff, size_t buffSize, uint64_t value, int *bytesWritten)
{
    int written = snprintf(buff, buffSize, "%" PRIu64, value);
    return checkWriteResult(written, buffSize, bytesWritten);
}

/** Add a formatted number to the JSON encoding context.
 *
 * @param context The JSON encoding context.
 * @param name The name of the element.
 * @param buff The buffer containing the formatted number.
 * @param written The number of characters in the formatted number.
 * @return TTSDKJSON_OK if successful, or an error code.
 */
static int addFormattedNumber(TTSDKJSONEncodeContext *const context, const char *const name, const char *buff, int written)
{
    int result = ttsdkjson_beginElement(context, name);
    unlikely_if(result != TTSDKJSON_OK) { return result; }
    return addJSONData(context, buff, written);
}

int ttsdkjson_beginElement(TTSDKJSONEncodeContext *const context, const char *const name)
{
    int result = TTSDKJSON_OK;

    // Decide if a comma is warranted.
    unlikely_if(context->containerFirstEntry) { context->containerFirstEntry = false; }
    else
    {
        unlikely_if((result = addJSONData(context, ",", 1)) != TTSDKJSON_OK) { return result; }
    }

    // Pretty printing
    unlikely_if(context->prettyPrint && context->containerLevel > 0)
    {
        unlikely_if((result = addJSONData(context, "\n", 1)) != TTSDKJSON_OK) { return result; }
        for (int i = 0; i < context->containerLevel; i++) {
            unlikely_if((result = addJSONData(context, "    ", 4)) != TTSDKJSON_OK) { return result; }
        }
    }

    // Add a name field if we're in an object.
    if (context->isObject[context->containerLevel]) {
        unlikely_if(name == NULL)
        {
            TTSDKLOG_DEBUG("Name was null inside an object");
            return TTSDKJSON_ERROR_INVALID_DATA;
        }
        unlikely_if((result = addQuotedEscapedString(context, name, (int)strlen(name))) != TTSDKJSON_OK) { return result; }
        unlikely_if(context->prettyPrint)
        {
            unlikely_if((result = addJSONData(context, ": ", 2)) != TTSDKJSON_OK) { return result; }
        }
        else
        {
            unlikely_if((result = addJSONData(context, ":", 1)) != TTSDKJSON_OK) { return result; }
        }
    }
    return result;
}

int ttsdkjson_addRawJSONData(TTSDKJSONEncodeContext *const context, const char *const data, const int length)
{
    return addJSONData(context, data, length);
}

int ttsdkjson_addBooleanElement(TTSDKJSONEncodeContext *const context, const char *const name, const bool value)
{
    int result = ttsdkjson_beginElement(context, name);
    unlikely_if(result != TTSDKJSON_OK) { return result; }
    if (value) {
        return addJSONData(context, "true", 4);
    } else {
        return addJSONData(context, "false", 5);
    }
}

int ttsdkjson_addFloatingPointElement(TTSDKJSONEncodeContext *const context, const char *const name, double value)
{
    char buff[64];
    int bytesWritten = 0;
    int result = formatDouble(buff, sizeof(buff), value, &bytesWritten);
    unlikely_if(result != TTSDKJSON_OK) { return result; }
    return addFormattedNumber(context, name, buff, bytesWritten);
}

int ttsdkjson_addIntegerElement(TTSDKJSONEncodeContext *const context, const char *const name, int64_t value)
{
    char buff[21];
    int bytesWritten = 0;
    int result = formatInt64(buff, sizeof(buff), value, &bytesWritten);
    unlikely_if(result != TTSDKJSON_OK) { return result; }
    return addFormattedNumber(context, name, buff, bytesWritten);
}

int ttsdkjson_addUIntegerElement(TTSDKJSONEncodeContext *const context, const char *const name, uint64_t value)
{
    char buff[21];
    int bytesWritten = 0;
    int result = formatUint64(buff, sizeof(buff), value, &bytesWritten);
    unlikely_if(result != TTSDKJSON_OK) { return result; }
    return addFormattedNumber(context, name, buff, bytesWritten);
}

int ttsdkjson_addNullElement(TTSDKJSONEncodeContext *const context, const char *const name)
{
    int result = ttsdkjson_beginElement(context, name);
    unlikely_if(result != TTSDKJSON_OK) { return result; }
    return addJSONData(context, "null", 4);
}

int ttsdkjson_addStringElement(TTSDKJSONEncodeContext *const context, const char *const name, const char *const value,
                            int length)
{
    unlikely_if(value == NULL) { return ttsdkjson_addNullElement(context, name); }
    int result = ttsdkjson_beginElement(context, name);
    unlikely_if(result != TTSDKJSON_OK) { return result; }
    if (length == TTSDKJSON_SIZE_AUTOMATIC) {
        length = (int)strlen(value);
    }
    return addQuotedEscapedString(context, value, length);
}

int ttsdkjson_beginStringElement(TTSDKJSONEncodeContext *const context, const char *const name)
{
    int result = ttsdkjson_beginElement(context, name);
    unlikely_if(result != TTSDKJSON_OK) { return result; }
    return addJSONData(context, "\"", 1);
}

int ttsdkjson_appendStringElement(TTSDKJSONEncodeContext *const context, const char *const value, int length)
{
    return addEscapedString(context, value, length);
}

int ttsdkjson_endStringElement(TTSDKJSONEncodeContext *const context) { return addJSONData(context, "\"", 1); }

int ttsdkjson_addDataElement(TTSDKJSONEncodeContext *const context, const char *name, const char *value, int length)
{
    int result = TTSDKJSON_OK;
    result = ttsdkjson_beginDataElement(context, name);
    if (result == TTSDKJSON_OK) {
        result = ttsdkjson_appendDataElement(context, value, length);
    }
    if (result == TTSDKJSON_OK) {
        result = ttsdkjson_endDataElement(context);
    }
    return result;
}

int ttsdkjson_beginDataElement(TTSDKJSONEncodeContext *const context, const char *const name)
{
    return ttsdkjson_beginStringElement(context, name);
}

int ttsdkjson_appendDataElement(TTSDKJSONEncodeContext *const context, const char *const value, int length)
{
    unsigned char *currentByte = (unsigned char *)value;
    unsigned char *end = currentByte + length;
    char chars[2];
    int result = TTSDKJSON_OK;
    while (currentByte < end) {
        chars[0] = g_hexNybbles[(*currentByte >> 4) & 15];
        chars[1] = g_hexNybbles[*currentByte & 15];
        result = addJSONData(context, chars, sizeof(chars));
        if (result != TTSDKJSON_OK) {
            break;
        }
        currentByte++;
    }
    return result;
}

int ttsdkjson_endDataElement(TTSDKJSONEncodeContext *const context) { return ttsdkjson_endStringElement(context); }

int ttsdkjson_beginArray(TTSDKJSONEncodeContext *const context, const char *const name)
{
    likely_if(context->containerLevel >= 0)
    {
        int result = ttsdkjson_beginElement(context, name);
        unlikely_if(result != TTSDKJSON_OK) { return result; }
    }

    context->containerLevel++;
    context->isObject[context->containerLevel] = false;
    context->containerFirstEntry = true;

    return addJSONData(context, "[", 1);
}

int ttsdkjson_beginObject(TTSDKJSONEncodeContext *const context, const char *const name)
{
    likely_if(context->containerLevel >= 0)
    {
        int result = ttsdkjson_beginElement(context, name);
        unlikely_if(result != TTSDKJSON_OK) { return result; }
    }

    context->containerLevel++;
    context->isObject[context->containerLevel] = true;
    context->containerFirstEntry = true;

    return addJSONData(context, "{", 1);
}

int ttsdkjson_endContainer(TTSDKJSONEncodeContext *const context)
{
    unlikely_if(context->containerLevel <= 0) { return TTSDKJSON_OK; }

    bool isObject = context->isObject[context->containerLevel];
    context->containerLevel--;

    // Pretty printing
    unlikely_if(context->prettyPrint && !context->containerFirstEntry)
    {
        int result;
        unlikely_if((result = addJSONData(context, "\n", 1)) != TTSDKJSON_OK) { return result; }
        for (int i = 0; i < context->containerLevel; i++) {
            unlikely_if((result = addJSONData(context, "    ", 4)) != TTSDKJSON_OK) { return result; }
        }
    }
    context->containerFirstEntry = false;
    return addJSONData(context, isObject ? "}" : "]", 1);
}

void ttsdkjson_beginEncode(TTSDKJSONEncodeContext *const context, bool prettyPrint, TTSDKJSONAddDataFunc addJSONDataFunc,
                        void *const userData)
{
    memset(context, 0, sizeof(*context));
    context->addJSONData = addJSONDataFunc;
    context->userData = userData;
    context->prettyPrint = prettyPrint;
    context->containerFirstEntry = true;
}

int ttsdkjson_endEncode(TTSDKJSONEncodeContext *const context)
{
    int result = TTSDKJSON_OK;
    while (context->containerLevel > 0) {
        unlikely_if((result = ttsdkjson_endContainer(context)) != TTSDKJSON_OK) { return result; }
    }
    return result;
}

// ============================================================================
#pragma mark - Decode -
// ============================================================================

#define INV 0x11111

typedef struct {
    /** Pointer to current work area in the buffer. */
    const char *bufferPtr;
    /** Pointer to the end of the buffer. */
    const char *bufferEnd;
    /** Pointer to a buffer for storing a decoded name. */
    char *nameBuffer;
    /** Length of the name buffer. */
    int nameBufferLength;
    /** Pointer to a buffer for storing a decoded string. */
    char *stringBuffer;
    /** Length of the string buffer. */
    int stringBufferLength;
    /** The callbacks to call while decoding. */
    TTSDKJSONDecodeCallbacks *const callbacks;
    /** Data that was specified when calling ttsdkjson_decode(). */
    void *userData;
} TTSDKJSONDecodeContext;

/** Lookup table for converting hex values to integers.
 * INV (0x11111) is used to mark invalid characters so that any attempted
 * invalid nybble conversion is always > 0xffff.
 */
static const unsigned int g_hexConversion[] = {
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, 0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, INV, INV, INV, INV, INV, INV, INV, 0xa,
    0xb, 0xc, 0xd, 0xe, 0xf, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
};

/** Encode a UTF-16 character to UTF-8. The dest pointer gets incremented
 * by however many bytes were needed for the conversion (1-4).
 *
 * @param character The UTF-16 character.
 *
 * @param dst Where to write the UTF-8 character.
 *
 * @return TTSDKJSON_OK if the encoding was successful.
 */
static int writeUTF8(unsigned int character, char **dst);

/** Decode a string value.
 *
 * @param context The decoding context.
 *
 * @param dstBuffer Buffer to hold the decoded string.
 *
 * @param dstBufferLength Length of the destination buffer.
 *
 * @return TTSDKJSON_OK if successful.
 */
static int decodeString(TTSDKJSONDecodeContext *context, char *dstBuffer, int dstBufferLength);

/** Decode a JSON element.
 *
 * @param name This element's name (or NULL if it has none).
 *
 * @param context The decoding context.
 *
 * @return TTSDKJSON_OK if successful.
 */
static int decodeElement(const char *const name, TTSDKJSONDecodeContext *context);

/** Skip past any whitespace.
 *
 * @param CONTEXT The decoding context.
 */
#define SKIP_WHITESPACE(CONTEXT)                                                      \
    while (CONTEXT->bufferPtr < CONTEXT->bufferEnd && isspace(*CONTEXT->bufferPtr)) { \
        CONTEXT->bufferPtr++;                                                         \
    }

/** Check if a character is valid for representing part of a floating point
 * number.
 *
 * @param ch The character to test.
 *
 * @return true if the character is valid for floating point.
 */
static inline bool isFPChar(char ch)
{
    switch (ch) {
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
        case '.':
        case 'e':
        case 'E':
        case '+':
        case '-':
            return true;
        default:
            return false;
    }
}

static int writeUTF8(unsigned int character, char **dst)
{
    likely_if(character <= 0x7f)
    {
        **dst = (char)character;
        (*dst)++;
        return TTSDKJSON_OK;
    }
    if (character <= 0x7ff) {
        (*dst)[0] = (char)(0xc0 | (character >> 6));
        (*dst)[1] = (char)(0x80 | (character & 0x3f));
        *dst += 2;
        return TTSDKJSON_OK;
    }
    if (character <= 0xffff) {
        (*dst)[0] = (char)(0xe0 | (character >> 12));
        (*dst)[1] = (char)(0x80 | ((character >> 6) & 0x3f));
        (*dst)[2] = (char)(0x80 | (character & 0x3f));
        *dst += 3;
        return TTSDKJSON_OK;
    }
    // RFC3629 restricts UTF-8 to end at 0x10ffff.
    if (character <= 0x10ffff) {
        (*dst)[0] = (char)(0xf0 | (character >> 18));
        (*dst)[1] = (char)(0x80 | ((character >> 12) & 0x3f));
        (*dst)[2] = (char)(0x80 | ((character >> 6) & 0x3f));
        (*dst)[3] = (char)(0x80 | (character & 0x3f));
        *dst += 4;
        return TTSDKJSON_OK;
    }

    // If we get here, the character cannot be converted to valid UTF-8.
    TTSDKLOG_DEBUG("Invalid unicode: 0x%04x", character);
    return TTSDKJSON_ERROR_INVALID_CHARACTER;
}

static int decodeString(TTSDKJSONDecodeContext *context, char *dstBuffer, int dstBufferLength)
{
    *dstBuffer = '\0';
    unlikely_if(*context->bufferPtr != '\"')
    {
        TTSDKLOG_DEBUG("Expected '\"' but got '%c'", *context->bufferPtr);
        return TTSDKJSON_ERROR_INVALID_CHARACTER;
    }

    const char *src = context->bufferPtr + 1;
    bool fastCopy = true;

    for (; src < context->bufferEnd && *src != '\"'; src++) {
        unlikely_if(*src == '\\')
        {
            fastCopy = false;
            src++;
        }
    }
    unlikely_if(src >= context->bufferEnd)
    {
        TTSDKLOG_DEBUG("Premature end of data");
        return TTSDKJSON_ERROR_INCOMPLETE;
    }
    const char *srcEnd = src;
    src = context->bufferPtr + 1;
    int length = (int)(srcEnd - src);
    if (length >= dstBufferLength) {
        TTSDKLOG_DEBUG("String is too long");
        return TTSDKJSON_ERROR_DATA_TOO_LONG;
    }

    context->bufferPtr = srcEnd + 1;

    // If no escape characters were encountered, we can fast copy.
    likely_if(fastCopy)
    {
        memcpy(dstBuffer, src, length);
        dstBuffer[length] = 0;
        return TTSDKJSON_OK;
    }

    char *dst = dstBuffer;

    for (; src < srcEnd; src++) {
        likely_if(*src != '\\') { *dst++ = *src; }
        else
        {
            src++;
            switch (*src) {
                case '"':
                    *dst++ = '\"';
                    continue;
                case '\\':
                    *dst++ = '\\';
                    continue;
                case 'n':
                    *dst++ = '\n';
                    continue;
                case 'r':
                    *dst++ = '\r';
                    continue;
                case '/':
                    *dst++ = '/';
                    continue;
                case 't':
                    *dst++ = '\t';
                    continue;
                case 'b':
                    *dst++ = '\b';
                    continue;
                case 'f':
                    *dst++ = '\f';
                    continue;
                case 'u': {
                    unlikely_if(src + 5 > srcEnd)
                    {
                        TTSDKLOG_DEBUG("Premature end of data");
                        return TTSDKJSON_ERROR_INCOMPLETE;
                    }
                    unsigned int accum = g_hexConversion[src[1]] << 12 | g_hexConversion[src[2]] << 8 |
                                         g_hexConversion[src[3]] << 4 | g_hexConversion[src[4]];
                    unlikely_if(accum > 0xffff)
                    {
                        TTSDKLOG_DEBUG("Invalid unicode sequence: %c%c%c%c", src[1], src[2], src[3], src[4]);
                        return TTSDKJSON_ERROR_INVALID_CHARACTER;
                    }

                    // UTF-16 Trail surrogate on its own.
                    unlikely_if(accum >= 0xdc00 && accum <= 0xdfff)
                    {
                        TTSDKLOG_DEBUG("Unexpected trail surrogate: 0x%04x", accum);
                        return TTSDKJSON_ERROR_INVALID_CHARACTER;
                    }

                    // UTF-16 Lead surrogate.
                    unlikely_if(accum >= 0xd800 && accum <= 0xdbff)
                    {
                        // Fetch trail surrogate.
                        unlikely_if(src + 11 > srcEnd)
                        {
                            TTSDKLOG_DEBUG("Premature end of data");
                            return TTSDKJSON_ERROR_INCOMPLETE;
                        }
                        unlikely_if(src[5] != '\\' || src[6] != 'u')
                        {
                            TTSDKLOG_DEBUG("Expected \"\\u\" but got: \"%c%c\"", src[5], src[6]);
                            return TTSDKJSON_ERROR_INVALID_CHARACTER;
                        }
                        src += 6;
                        unsigned int accum2 = g_hexConversion[src[1]] << 12 | g_hexConversion[src[2]] << 8 |
                                              g_hexConversion[src[3]] << 4 | g_hexConversion[src[4]];
                        unlikely_if(accum2 < 0xdc00 || accum2 > 0xdfff)
                        {
                            TTSDKLOG_DEBUG("Invalid trail surrogate: 0x%04x", accum2);
                            return TTSDKJSON_ERROR_INVALID_CHARACTER;
                        }
                        // And combine 20 bit result.
                        accum = ((accum - 0xd800) << 10) | (accum2 - 0xdc00);
                    }

                    int result = writeUTF8(accum, &dst);
                    unlikely_if(result != TTSDKJSON_OK) { return result; }
                    src += 4;
                    continue;
                }
                default:
                    TTSDKLOG_DEBUG("Invalid control character '%c'", *src);
                    return TTSDKJSON_ERROR_INVALID_CHARACTER;
            }
        }
    }

    *dst = 0;
    return TTSDKJSON_OK;
}

static int decodeElement(const char *const name, TTSDKJSONDecodeContext *context)
{
    SKIP_WHITESPACE(context);
    unlikely_if(context->bufferPtr >= context->bufferEnd)
    {
        TTSDKLOG_DEBUG("Premature end of data");
        return TTSDKJSON_ERROR_INCOMPLETE;
    }

    int sign = 1;
    int result;

    switch (*context->bufferPtr) {
        case '[': {
            context->bufferPtr++;
            result = context->callbacks->onBeginArray(name, context->userData);
            unlikely_if(result != TTSDKJSON_OK) return result;
            while (context->bufferPtr < context->bufferEnd) {
                SKIP_WHITESPACE(context);
                unlikely_if(context->bufferPtr >= context->bufferEnd) { break; }
                unlikely_if(*context->bufferPtr == ']')
                {
                    context->bufferPtr++;
                    return context->callbacks->onEndContainer(context->userData);
                }
                result = decodeElement(NULL, context);
                unlikely_if(result != TTSDKJSON_OK) return result;
                SKIP_WHITESPACE(context);
                unlikely_if(context->bufferPtr >= context->bufferEnd) { break; }
                likely_if(*context->bufferPtr == ',') { context->bufferPtr++; }
            }
            TTSDKLOG_DEBUG("Premature end of data");
            return TTSDKJSON_ERROR_INCOMPLETE;
        }
        case '{': {
            context->bufferPtr++;
            result = context->callbacks->onBeginObject(name, context->userData);
            unlikely_if(result != TTSDKJSON_OK) return result;
            while (context->bufferPtr < context->bufferEnd) {
                SKIP_WHITESPACE(context);
                unlikely_if(context->bufferPtr >= context->bufferEnd) { break; }
                unlikely_if(*context->bufferPtr == '}')
                {
                    context->bufferPtr++;
                    return context->callbacks->onEndContainer(context->userData);
                }
                result = decodeString(context, context->nameBuffer, context->nameBufferLength);
                unlikely_if(result != TTSDKJSON_OK) return result;
                SKIP_WHITESPACE(context);
                unlikely_if(context->bufferPtr >= context->bufferEnd) { break; }
                unlikely_if(*context->bufferPtr != ':')
                {
                    TTSDKLOG_DEBUG("Expected ':' but got '%c'", *context->bufferPtr);
                    return TTSDKJSON_ERROR_INVALID_CHARACTER;
                }
                context->bufferPtr++;
                SKIP_WHITESPACE(context);
                result = decodeElement(context->nameBuffer, context);
                unlikely_if(result != TTSDKJSON_OK) return result;
                SKIP_WHITESPACE(context);
                unlikely_if(context->bufferPtr >= context->bufferEnd) { break; }
                likely_if(*context->bufferPtr == ',') { context->bufferPtr++; }
            }
            TTSDKLOG_DEBUG("Premature end of data");
            return TTSDKJSON_ERROR_INCOMPLETE;
        }
        case '\"': {
            result = decodeString(context, context->stringBuffer, context->stringBufferLength);
            unlikely_if(result != TTSDKJSON_OK) return result;
            result = context->callbacks->onStringElement(name, context->stringBuffer, context->userData);
            return result;
        }
        case 'f': {
            unlikely_if(context->bufferEnd - context->bufferPtr < 5)
            {
                TTSDKLOG_DEBUG("Premature end of data");
                return TTSDKJSON_ERROR_INCOMPLETE;
            }
            unlikely_if(!(context->bufferPtr[1] == 'a' && context->bufferPtr[2] == 'l' &&
                          context->bufferPtr[3] == 's' && context->bufferPtr[4] == 'e'))
            {
                TTSDKLOG_DEBUG("Expected \"false\" but got \"f%c%c%c%c\"", context->bufferPtr[1], context->bufferPtr[2],
                            context->bufferPtr[3], context->bufferPtr[4]);
                return TTSDKJSON_ERROR_INVALID_CHARACTER;
            }
            context->bufferPtr += 5;
            return context->callbacks->onBooleanElement(name, false, context->userData);
        }
        case 't': {
            unlikely_if(context->bufferEnd - context->bufferPtr < 4)
            {
                TTSDKLOG_DEBUG("Premature end of data");
                return TTSDKJSON_ERROR_INCOMPLETE;
            }
            unlikely_if(!(context->bufferPtr[1] == 'r' && context->bufferPtr[2] == 'u' && context->bufferPtr[3] == 'e'))
            {
                TTSDKLOG_DEBUG("Expected \"true\" but got \"t%c%c%c\"", context->bufferPtr[1], context->bufferPtr[2],
                            context->bufferPtr[3]);
                return TTSDKJSON_ERROR_INVALID_CHARACTER;
            }
            context->bufferPtr += 4;
            return context->callbacks->onBooleanElement(name, true, context->userData);
        }
        case 'n': {
            unlikely_if(context->bufferEnd - context->bufferPtr < 4)
            {
                TTSDKLOG_DEBUG("Premature end of data");
                return TTSDKJSON_ERROR_INCOMPLETE;
            }
            unlikely_if(!(context->bufferPtr[1] == 'u' && context->bufferPtr[2] == 'l' && context->bufferPtr[3] == 'l'))
            {
                TTSDKLOG_DEBUG("Expected \"null\" but got \"n%c%c%c\"", context->bufferPtr[1], context->bufferPtr[2],
                            context->bufferPtr[3]);
                return TTSDKJSON_ERROR_INVALID_CHARACTER;
            }
            context->bufferPtr += 4;
            return context->callbacks->onNullElement(name, context->userData);
        }
        case '-':
            sign = -1;
            context->bufferPtr++;
            unlikely_if(!isdigit(*context->bufferPtr))
            {
                TTSDKLOG_DEBUG("Not a digit: '%c'", *context->bufferPtr);
                return TTSDKJSON_ERROR_INVALID_CHARACTER;
            }
            // Fall through
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9': {
            // Try integer conversion.
            uint64_t accum = 0;
            bool isOverflow = false;
            const char *const start = context->bufferPtr;

            for (; context->bufferPtr < context->bufferEnd && isdigit(*context->bufferPtr); context->bufferPtr++) {
                unlikely_if((isOverflow = accum > (ULLONG_MAX / 10))) { break; }
                accum *= 10;
                uint64_t nextDigit = (uint64_t)(*context->bufferPtr - '0');
                unlikely_if((isOverflow = accum > (ULLONG_MAX - nextDigit))) { break; }
                accum += nextDigit;
            }

            unlikely_if(context->bufferPtr >= context->bufferEnd)
            {
                TTSDKLOG_DEBUG("Premature end of data");
                return TTSDKJSON_ERROR_INCOMPLETE;
            }

            if (!isFPChar(*context->bufferPtr) && !isOverflow) {
                if (sign > 0) {
                    if (accum <= (uint64_t)LLONG_MAX) {
                        // Positive number within int64_t range
                        return context->callbacks->onIntegerElement(name, (int64_t)accum, context->userData);
                    } else {
                        // Positive number exceeding int64_t range, use unsigned
                        return context->callbacks->onUnsignedIntegerElement(name, accum, context->userData);
                    }
                } else {
                    if (accum <= ((uint64_t)LLONG_MAX + 1)) {
                        // Negative number within int64_t range
                        int64_t signedAccum = -(int64_t)accum;
                        return context->callbacks->onIntegerElement(name, signedAccum, context->userData);
                    }
                    // If negative and exceeding int64_t range, fall through to floating point
                }
            }

            while (context->bufferPtr < context->bufferEnd && isFPChar(*context->bufferPtr)) {
                context->bufferPtr++;
            }

            unlikely_if(context->bufferPtr >= context->bufferEnd)
            {
                TTSDKLOG_DEBUG("Premature end of data");
                return TTSDKJSON_ERROR_INCOMPLETE;
            }

            // our buffer is not necessarily NULL-terminated, so
            // it would be undefined to call sscanf/sttod etc. directly.
            // instead we create a temporary string.
            double value;
            int len = (int)(context->bufferPtr - start);
            if (len >= context->stringBufferLength) {
                TTSDKLOG_DEBUG("Number is too long.");
                return TTSDKJSON_ERROR_DATA_TOO_LONG;
            }
            strncpy(context->stringBuffer, start, len);
            context->stringBuffer[len] = '\0';

            sscanf(context->stringBuffer, "%lg", &value);

            value *= sign;
            return context->callbacks->onFloatingPointElement(name, value, context->userData);
        }
    }
    TTSDKLOG_DEBUG("Invalid character '%c'", *context->bufferPtr);
    return TTSDKJSON_ERROR_INVALID_CHARACTER;
}

int ttsdkjson_decode(const char *const data, int length, char *stringBuffer, int stringBufferLength,
                  TTSDKJSONDecodeCallbacks *const callbacks, void *const userData, int *const errorOffset)
{
    char *nameBuffer = stringBuffer;
    int nameBufferLength = stringBufferLength / 4;
    stringBuffer = nameBuffer + nameBufferLength;
    stringBufferLength -= nameBufferLength;
    TTSDKJSONDecodeContext context = { .bufferPtr = (char *)data,
                                    .bufferEnd = (char *)data + length,
                                    .nameBuffer = nameBuffer,
                                    .nameBufferLength = nameBufferLength,
                                    .stringBuffer = stringBuffer,
                                    .stringBufferLength = (int)stringBufferLength,
                                    .callbacks = callbacks,
                                    .userData = userData };

    const char *ptr = data;

    int result = decodeElement(NULL, &context);
    likely_if(result == TTSDKJSON_OK) { result = callbacks->onEndData(userData); }

    unlikely_if(result != TTSDKJSON_OK && errorOffset != NULL) { *errorOffset = (int)(ptr - data); }
    return result;
}

struct JSONFromFileContext;
typedef void (*UpdateDecoderCallback)(struct JSONFromFileContext *context);

typedef struct JSONFromFileContext {
    TTSDKJSONEncodeContext *encodeContext;
    TTSDKJSONDecodeContext *decodeContext;
    char *bufferStart;
    const char *sourceFilename;
    int fd;
    bool isEOF;
    bool closeLastContainer;
    UpdateDecoderCallback updateDecoderCallback;
} JSONFromFileContext;

static void updateDecoder_doNothing(__unused struct JSONFromFileContext *context) {}

static void updateDecoder_readFile(struct JSONFromFileContext *context)
{
    likely_if(!context->isEOF)
    {
        const char *end = context->decodeContext->bufferEnd;
        char *start = context->bufferStart;
        const char *ptr = context->decodeContext->bufferPtr;
        int bufferLength = (int)(end - start);
        int remainingLength = (int)(end - ptr);
        unlikely_if(remainingLength < bufferLength / 2)
        {
            int fillLength = bufferLength - remainingLength;
            memcpy(start, ptr, remainingLength);
            context->decodeContext->bufferPtr = start;
            int bytesRead = (int)read(context->fd, start + remainingLength, (unsigned)fillLength);
            unlikely_if(bytesRead < fillLength)
            {
                if (bytesRead < 0) {
                    TTSDKLOG_ERROR("Error reading file %s: %s", context->sourceFilename, strerror(errno));
                }
                context->isEOF = true;
            }
        }
    }
}

static int addJSONFromFile_onBooleanElement(const char *const name, const bool value, void *const userData)
{
    JSONFromFileContext *context = (JSONFromFileContext *)userData;
    int result = ttsdkjson_addBooleanElement(context->encodeContext, name, value);
    context->updateDecoderCallback(context);
    return result;
}

static int addJSONFromFile_onFloatingPointElement(const char *const name, const double value, void *const userData)
{
    JSONFromFileContext *context = (JSONFromFileContext *)userData;
    int result = ttsdkjson_addFloatingPointElement(context->encodeContext, name, value);
    context->updateDecoderCallback(context);
    return result;
}

static int addJSONFromFile_onIntegerElement(const char *const name, const int64_t value, void *const userData)
{
    JSONFromFileContext *context = (JSONFromFileContext *)userData;
    int result = ttsdkjson_addIntegerElement(context->encodeContext, name, value);
    context->updateDecoderCallback(context);
    return result;
}

static int addJSONFromFile_onUnsignedIntegerElement(const char *const name, const uint64_t value, void *const userData)
{
    JSONFromFileContext *context = (JSONFromFileContext *)userData;
    int result = ttsdkjson_addUIntegerElement(context->encodeContext, name, value);
    context->updateDecoderCallback(context);
    return result;
}

static int addJSONFromFile_onNullElement(const char *const name, void *const userData)
{
    JSONFromFileContext *context = (JSONFromFileContext *)userData;
    int result = ttsdkjson_addNullElement(context->encodeContext, name);
    context->updateDecoderCallback(context);
    return result;
}

static int addJSONFromFile_onStringElement(const char *const name, const char *const value, void *const userData)
{
    JSONFromFileContext *context = (JSONFromFileContext *)userData;
    int result = ttsdkjson_addStringElement(context->encodeContext, name, value, (int)strlen(value));
    context->updateDecoderCallback(context);
    return result;
}

static int addJSONFromFile_onBeginObject(const char *const name, void *const userData)
{
    JSONFromFileContext *context = (JSONFromFileContext *)userData;
    int result = ttsdkjson_beginObject(context->encodeContext, name);
    context->updateDecoderCallback(context);
    return result;
}

static int addJSONFromFile_onBeginArray(const char *const name, void *const userData)
{
    JSONFromFileContext *context = (JSONFromFileContext *)userData;
    int result = ttsdkjson_beginArray(context->encodeContext, name);
    context->updateDecoderCallback(context);
    return result;
}

static int addJSONFromFile_onEndContainer(void *const userData)
{
    JSONFromFileContext *context = (JSONFromFileContext *)userData;
    int result = TTSDKJSON_OK;
    if (context->closeLastContainer || context->encodeContext->containerLevel > 2) {
        result = ttsdkjson_endContainer(context->encodeContext);
    }
    context->updateDecoderCallback(context);
    return result;
}

static int addJSONFromFile_onEndData(__unused void *const userData) { return TTSDKJSON_OK; }

int ttsdkjson_addJSONFromFile(TTSDKJSONEncodeContext *const encodeContext, const char *restrict const name,
                           const char *restrict const filename, const bool closeLastContainer)
{
    TTSDKJSONDecodeCallbacks callbacks = {
        .onBeginArray = addJSONFromFile_onBeginArray,
        .onBeginObject = addJSONFromFile_onBeginObject,
        .onBooleanElement = addJSONFromFile_onBooleanElement,
        .onEndContainer = addJSONFromFile_onEndContainer,
        .onEndData = addJSONFromFile_onEndData,
        .onFloatingPointElement = addJSONFromFile_onFloatingPointElement,
        .onIntegerElement = addJSONFromFile_onIntegerElement,
        .onUnsignedIntegerElement = addJSONFromFile_onUnsignedIntegerElement,
        .onNullElement = addJSONFromFile_onNullElement,
        .onStringElement = addJSONFromFile_onStringElement,
    };
    char nameBuffer[100] = { 0 };
    char stringBuffer[500] = { 0 };
    char fileBuffer[1000] = { 0 };
    TTSDKJSONDecodeContext decodeContext = {
        .bufferPtr = fileBuffer,
        .bufferEnd = fileBuffer + sizeof(fileBuffer),
        .nameBuffer = nameBuffer,
        .nameBufferLength = sizeof(nameBuffer),
        .stringBuffer = stringBuffer,
        .stringBufferLength = sizeof(stringBuffer),
        .callbacks = &callbacks,
        .userData = NULL,
    };

    int fd = open(filename, O_RDONLY);
    JSONFromFileContext jsonContext = {
        .encodeContext = encodeContext,
        .decodeContext = &decodeContext,
        .bufferStart = fileBuffer,
        .sourceFilename = filename,
        .fd = fd,
        .closeLastContainer = closeLastContainer,
        .isEOF = false,
        .updateDecoderCallback = updateDecoder_readFile,
    };
    decodeContext.userData = &jsonContext;
    int containerLevel = encodeContext->containerLevel;

    // Manually trigger a data load.
    decodeContext.bufferPtr = decodeContext.bufferEnd;
    jsonContext.updateDecoderCallback(&jsonContext);

    int result = decodeElement(name, &decodeContext);
    close(fd);
    while (closeLastContainer && encodeContext->containerLevel > containerLevel) {
        ttsdkjson_endContainer(encodeContext);
    }

    return result;
}

int ttsdkjson_addJSONElement(TTSDKJSONEncodeContext *const encodeContext, const char *restrict const name,
                          const char *restrict const jsonData, const int jsonDataLength, const bool closeLastContainer)
{
    TTSDKJSONDecodeCallbacks callbacks = {
        .onBeginArray = addJSONFromFile_onBeginArray,
        .onBeginObject = addJSONFromFile_onBeginObject,
        .onBooleanElement = addJSONFromFile_onBooleanElement,
        .onEndContainer = addJSONFromFile_onEndContainer,
        .onEndData = addJSONFromFile_onEndData,
        .onFloatingPointElement = addJSONFromFile_onFloatingPointElement,
        .onIntegerElement = addJSONFromFile_onIntegerElement,
        .onUnsignedIntegerElement = addJSONFromFile_onUnsignedIntegerElement,
        .onNullElement = addJSONFromFile_onNullElement,
        .onStringElement = addJSONFromFile_onStringElement,
    };
    char nameBuffer[100] = { 0 };
    char stringBuffer[5000] = { 0 };
    TTSDKJSONDecodeContext decodeContext = {
        .bufferPtr = jsonData,
        .bufferEnd = jsonData + jsonDataLength,
        .nameBuffer = nameBuffer,
        .nameBufferLength = sizeof(nameBuffer),
        .stringBuffer = stringBuffer,
        .stringBufferLength = sizeof(stringBuffer),
        .callbacks = &callbacks,
        .userData = NULL,
    };

    JSONFromFileContext jsonContext = {
        .encodeContext = encodeContext,
        .decodeContext = &decodeContext,
        .bufferStart = (char *)jsonData,
        .sourceFilename = NULL,
        .fd = 0,
        .closeLastContainer = closeLastContainer,
        .isEOF = false,
        .updateDecoderCallback = updateDecoder_doNothing,
    };
    decodeContext.userData = &jsonContext;
    int containerLevel = encodeContext->containerLevel;

    int result = decodeElement(name, &decodeContext);
    while (closeLastContainer && encodeContext->containerLevel > containerLevel) {
        ttsdkjson_endContainer(encodeContext);
    }

    return result;
}
