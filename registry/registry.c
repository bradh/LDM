/*
 *   See file ../COPYRIGHT for copying and redistribution conditions.
 *
 *   This file implements the API for the registry.
 *
 *   This module hides the decision on how to implement the persistent store.
 *
 *   The functions in this file are thread-compatible but not thread-safe.
 */
#include <config.h>

#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <search.h>

#include "backend.h"
#include <ldmprint.h>
#include <log.h>
#include "misc.h"
#include "node.h"
#include "registry.h"
#include "stringBuf.h"
#include <timestamp.h>

#define NOT_SYNCHED     0
#define SYNCHED         1

struct regCursor {
};

/*
 * Parses a string into a value.
 *
 * Arguments:
 *      string          Pointer to the string to be "parsed".  Shall not be
 *                      NULL.
 *      value           Pointer to the value.  Shall not be NULL.
 * Returns:
 *      0               Success
 *      ENOMEM          System error.  "log_start()" called.
 *      EILSEQ          The string isn't a valid representation of the type
 */
typedef RegStatus (*Parser)(const char* string, void* value);
/*
 * Formats a value into a string.
 *
 * Arguments:
 *      value           Pointer to the value to be formatted.  Shall not be
 *                      NULL.
 *      strBuf          Pointer to a string-buffer to receive the formatted
 *                      representation of the value.  Shall not be NULL.
 * Returns:
 *      0               Success.  "*strBuf" is not NULL.
 *      ENOMEM          System error.  "log_start()" called.
 */
typedef RegStatus (*Formatter)(const void* value, StringBuf* strBuf);

typedef enum {
    /* Start with 0 to enable use as an index: */
    REG_STRING = 0,
    REG_UINT,
    REG_TIME,
    REG_SIGNATURE,
    /* Keep at end to track the number of types: */
    REG_TYPECOUNT
}       RegType;

typedef struct {
    Parser      parse;
    Formatter   format;
}       TypeStruct;

static char*                    _registryPath = REGISTRY_PATH;
static int                      _initialized;   /* Module is initialized? */
static int                      _atexitCalled;  /* atexit() called? */
static Backend*                 _backend;       /* backend database */
static int                      _forWriting;    /* registry open for writing? */
static StringBuf*               _pathBuf;       /* buffer for creating paths */
static StringBuf*               _formatBuf;     /* values-formatting buffer */
static const TypeStruct*        _typeStructs[REG_TYPECOUNT];
static RegNode*                 _rootNode;
static ValueFunc                _extantValueFunc;
static const char*              _nodePath;      /* pathname of visited node */
static StringBuf*               _valuePath;     /* pathname of visited value */

/******************************************************************************
 * Private Functions:
 ******************************************************************************/

/*
 * Resets this module
 */
static void resetRegistry(void)
{
    if (NULL != _pathBuf) {
        sb_free(_pathBuf);
        _pathBuf = NULL;
    }
    if (NULL != _formatBuf) {
        sb_free(_formatBuf);
        _formatBuf = NULL;
    }
    if (NULL != _valuePath) {
        sb_free(_valuePath);
        _valuePath = NULL;
    }
    if (NULL != _rootNode) {
        rn_free(_rootNode);
        _rootNode = NULL;
    }
    if (REGISTRY_PATH != _registryPath) {
        free(_registryPath);
        _registryPath = REGISTRY_PATH;
    }
    _initialized = 0;
    _backend = NULL;
    _forWriting = 0;
}

/*
 * Closes the registry if it's open.  Doesn't reset this module, however.
 *
 * Returns:
 *      0               Success.
 *      EIO             Backend database error.  "log_start()" called.
 */
static RegStatus closeRegistry(void)
{
    RegStatus   status;

    if (NULL == _backend) {
        status = 0;
    }
    else {
        status = beClose(_backend);
        _backend = NULL;
    }

    return status;
}

/*
 * Closes the registry if it's open.  This function shall only be called by
 * exit().
 */
static void terminate(void)
{
    (void)closeRegistry();
    resetRegistry();
}

/*
 * "Parses" a string into a string.
 *
 * Arguments:
 *      string          Pointer to the string to be "parsed".  Shall not be
 *                      NULL.
 *      value           Pointer to the value.  Shall not be NULL.
 * Returns:
 *      0               Success
 */
static RegStatus parseString(
    const char* const   string,
    void* const         value)
{
    return reg_cloneString((char**)value, string);
}

/*
 * "Formats" a string into a string.
 *
 * Arguments:
 *      value           Pointer to the value to be formatted.  Shall not be
 *                      NULL.
 *      strBuf          Pointer to a string-buffer to receive the formatted
 *                      representation of the value.  Shall not be NULL.
 * Returns:
 *      0               Success.  The string-buffer is not NULL.
 *      ENOMEM   System error.  "log_start()" called.
 */
static RegStatus formatString(
    const void* const   value,
    StringBuf* const    strBuf)
{
    return sb_set(strBuf, (const char*)value, NULL);
}

/*
 * Parses a string into an unsigned integer.
 *
 * Arguments:
 *      string          Pointer to the string to be parsed.  Shall not be NULL.
 *      value           Pointer to the value.  Shall not be NULL.
 * Returns:
 *      0               Success
 *      EILSEQ          The string doesn't represent an unsigned integer.
 *                      "log_start()" called.
 */
static RegStatus parseUint(
    const char* const   string,
    void* const         value)
{
    RegStatus           status;
    char*               end;
    unsigned long       val;

    errno = 0;
    val = strtoul(string, &end, 0);

    if (0 != *end || (0 == val && 0 != errno)) {
        log_start("Not an unsigned integer: \"%s\"", string);
        status = EILSEQ;
    }
    else {
        *(unsigned*)value = (unsigned)val;
        status = 0;
    }

    return status;
}

/*
 * Formats an unsigned integer into a string.
 *
 * Arguments:
 *      value           Pointer to the value to be formatted
 *      strBuf          Pointer to a string-buffer to receive the formatted
 *                      representation of the value.  Shall not be NULL.
 * Returns:
 *      0               Success.  "*value" is not NULL.
 *      ENOMEM   System error.  "log_start()" called.
 */
static RegStatus formatUint(
    const void* const   value,
    StringBuf* const    strBuf)
{
    static char buf[80];

    (void)snprintf(buf, sizeof(buf)-1, "%u", *(unsigned*)value);

    return sb_set(strBuf, buf, NULL);
}

/*
 * Parses a string into a time.
 *
 * Arguments:
 *      string          Pointer to the string to be parsed.  Shall not be NULL.
 *      value           Pointer to the value.  Shall not be NULL.
 * Returns:
 *      0               Success
 *      EILSEQ          The string doesn't represent a time
 */
static RegStatus parseTime(
    const char* const   string,
    void* const         value)
{
    RegStatus   status;
    timestampt  val;

    status = tsParse(string, &val);

    if (0 > status || 0 != string[status]) {
        log_start("Not a timestamp: \"%s\"", string);
        status = EILSEQ;
    }
    else {
        *(timestampt*)value = val;
        status = 0;
    }

    return status;
}

/*
 * Formats a time into a string.
 *
 * Arguments:
 *      value           Pointer to the value to be formatted
 *      strBuf          Pointer to a string-buffer to receive the formatted
 *                      representation of the value.  Shall not be NULL.
 * Returns:
 *      0               Success.  "*strBuf" is not NULL.
 *      ENOMEM   System error.  "log_start()" called.
 */
static RegStatus formatTime(
    const void* const   value,
    StringBuf* const    strBuf)
{
    return sb_set(strBuf, tsFormat((const timestampt*)value), NULL);
}

/*
 * Parses a string into a signature.
 *
 * Arguments:
 *      string          Pointer to the string to be parsed.  Shall not be NULL.
 *      value           Pointer to the value.  Shall not be NULL.
 * Returns:
 *      0               Success
 *      EILSEQ          The string doesn't represent a signature
 */
static RegStatus parseSignature(
    const char* const   string,
    void* const         value)
{
    RegStatus   status;
    signaturet  val;

    status = sigParse(string, &val);

    if (0 > status || 0 != string[status]) {
        log_start("Not a signature: \"%s\"", string);
        status = EILSEQ;
    }
    else {
        (void)memcpy(value, val, sizeof(signaturet));
        status = 0;
    }

    return status;
}

/*
 * Formats a signature into a string.
 *
 * Arguments:
 *      value           Pointer to the value to be formatted
 *      strBuf          Pointer to a string-buffer to receive the formatted
 *                      representation of the value.  Shall not be NULL.
 * Returns:
 *      0               Success.  "*strBuf" is not NULL.
 *      ENOMEM   System error.  "log_start()" called.
 */
static RegStatus formatSignature(
    const void* const   value,
    StringBuf* const    strBuf)
{
    return sb_set(strBuf, s_signaturet(NULL, 0, value),
        NULL);
}

static const TypeStruct  stringStruct = {parseString, formatString};
static const TypeStruct  uintStruct = {parseUint, formatUint};
static const TypeStruct  timeStruct = {parseTime, formatTime};
static const TypeStruct  signatureStruct = {parseSignature, formatSignature};

static RegStatus sync(
    RegNode* const      node);

/*
 * Initializes the registry.  Ensures that the backend is open for the desired
 * access.  Registers the process termination function.  May be called many
 * times.
 *
 * Arguments:
 *      forWriting      Indicates if the backend should be open for writing:
 *                      0 <=> no.
 * Returns:
 *      0               Success
 *      EIO    Backend database error.  "log_start()" called.
 *      ENOMEM   System error.  "log_start()" called.
 */
static RegStatus initRegistry(
    const int   forWriting)
{
    RegStatus   status = 0;             /* success */
 
    if (!_initialized) {
        if (0 != sb_new(&_pathBuf, 80)) {
            log_add("Couldn't allocate path-buffer");
            status = ENOMEM;
        }
        else {
            if (0 != sb_new(&_formatBuf, 80)) {
                log_add("Couldn't allocate formating-buffer");
                status = ENOMEM;
            }
            else {
                if (0 != sb_new(&_valuePath, 80)) {
                    log_add("Couldn't allocate value-pathname buffer");
                    status = ENOMEM;
                }
                else {
                    _typeStructs[REG_STRING] = &stringStruct;
                    _typeStructs[REG_UINT] = &uintStruct;
                    _typeStructs[REG_TIME] = &timeStruct;
                    _typeStructs[REG_SIGNATURE] = &signatureStruct;
                    _initialized = 1;
                    status = 0;
                }                   /* "_valuePath" allocated */

                if (status) {
                    sb_free(_formatBuf);
                    _formatBuf = NULL;
                }
            }                       /* "_formatBuf" allocated */

            if (status) {
                sb_free(_pathBuf);
                _pathBuf = NULL;
            }
        }                           /* "_pathBuf" allocated */
    }                               /* module not initialized */
 
    if (0 == status) {
        if (NULL != _backend && forWriting && !_forWriting) {
            /* The backend is open for the wrong (read-only) access */
            status = beClose(_backend);
            _backend = NULL;
        }
 
        if (0 == status && NULL == _backend) {
            /* The backend isn't open. */
            if (0 != (status = beOpen(&_backend, _registryPath, forWriting))) {
                log_add("Couldn't open registry \"%s\"", _registryPath);
            }
            else {
                _forWriting = forWriting;
 
                if (NULL == _rootNode) {
                    RegNode*    root;

                    if (0 == (status = rn_newRoot(&root))) {
                        if (0 == (status = sync(root)))
                            _rootNode = root;
                    }
                }
                if (status) {
                    beClose(_backend);
                    _backend = NULL;
                }
            }                           /* "_backend" allocated and open */
        }                               /* backend database not open */
    }                                   /* module initialized */

    if (!_atexitCalled) {
        if (0 == atexit(terminate)) {
            _atexitCalled = 1;
        }
        else {
            log_serror("Couldn't register registry cleanup routine");
            log_log(LOG_ERR);
        }
    }
 
    return status;
}

/*
 * Forms the absolute path name of a value.
 *
 * Arguments:
 *      sb              Pointer to a string-buffer.  Shall not be NULL.
 *      nodePath        Pointer to the absolute path name of the containing
 *                      node.  Shall not be NULL.
 *      vt              Pointer to the value-thing.  Shall not be NULL.
 * Returns:
 *      0               Success
 *      ENOMEM          System error.  "log_start()" called.
 */
static int formAbsValuePath(
    StringBuf* const            sb,
    const char* const           nodePath,
    const ValueThing* const     vt)
{
    return sb_set(sb, reg_isAbsRootPath(nodePath) ? "" : nodePath, REG_SEP,
        vt_getName(vt), NULL);
}

/*
 * Writes a value to the backend database.
 *
 * Arguments:
 *      vt              Pointer to the ValueThing.  Shall not be NULL.
 * Returns:
 *      0               Success
 *      ENOMEM          System error.  "log_start()" called.
 *      EIO             Backend database error.  "log_start()" called.
 */
static RegStatus writeValue(
    ValueThing* const   vt)
{
    RegStatus   status;

    if (SYNCHED == vt_getStatus(vt)) {
        status = 0;
    }
    else {
        if (0 == (status = formAbsValuePath(_valuePath, _nodePath, vt))) {
            if (0 == (status = bePut(_backend, sb_string(_valuePath),
                    vt_getValue(vt)))) {
                (void)vt_setStatus(vt, SYNCHED);
            }
        }
    }

    return status;
}

/*
 * Deletes a value from the backend database.
 *
 * Arguments:
 *      vt              Pointer to the ValueThing.  Shall not be NULL.
 * Returns:
 *      0               Success
 *      ENOMEM          System error.  "log_start()" called.
 *      EIO             Backend database error.  "log_start()" called.
 */
static RegStatus deleteValue(
    ValueThing* const     vt)
{
    RegStatus   status = formAbsValuePath(_valuePath, _nodePath, vt);

    if (0 == status)
        status = beDelete(_backend, sb_string(_valuePath));

    return status;
}

/*
 * Writes a node to the backend database.
 *
 * Arguments:
 *      node            Pointer to the node to be written.  Shall not be NULL.
 * Returns:
 *      0               Success
 *      EIO             Backend database error.  "log_start()" called.
 */
static RegStatus writeNode(
    RegNode*    node)
{
    RegStatus   status;

    if (rn_isDeleted(node))
        _extantValueFunc = deleteValue;

    _nodePath = rn_getAbsPath(node);

    if (0 == (status = rn_visitValues(node, _extantValueFunc, deleteValue)))
        rn_freeDeletedValues(node);

    if (0 != status)
        log_add("Couldn't update values of node \"%s\"", rn_getAbsPath(node));

    return status;
}

/*
 * Flushes a node and all its descendents to the backend database.
 *
 * Arguments:
 *      node            Pointer to the node to be flushed along with all its
 *                      descendents.  Shall not be NULL.
 * Returns:
 *      0               Success
 *      EIO             Backend database error.  "log_start()" called.
 */
static RegStatus flush(
    RegNode* const      node)
{
    _extantValueFunc = writeValue;

    return rn_visitNodes(node, writeNode);
}

/*
 * Synchronizes a node and its descendants from the backend database.
 *
 * Arguments:
 *      node            Pointer to the node to have it and its descendants
 *                      synchronized from the backend database.  Shall not be
 *                      NULL.
 * Returns:
 *      0               Success
 *      ENOMEM          System error.  "log_start()" called.
 *      EIO             Backend database error.  "log_start()" called.
 */
static RegStatus sync(
    RegNode* const      node)
{
    const char* absPath = rn_getAbsPath(node);
    RegStatus   status;
    RdbCursor   cursor;

    rn_clear(node);

    if (0 == (status = beInitCursor(_backend, &cursor))) {
        for (status = beFirstEntry(&cursor, absPath); 0 == status;
                status = beNextEntry(&cursor)) {
            if (strstr(cursor.key, absPath) != cursor.key) {
                /* The entry is outside the scope of "node" */
                status = ENOENT;
                break;
            }
            else {
                char* relPath;
                char* name;

                if (0 == (status = reg_splitAbsPath(cursor.key, absPath,
                        &relPath, &name))) {
                    RegNode*        subnode;

                    if (0 == (status = rn_ensure(node, relPath, &subnode))) {
                        ValueThing* vt;

                        if (0 == (status = rn_putValue(subnode, name,
                                cursor.value, &vt))) {
                            (void)vt_setStatus(vt, SYNCHED);
                        }
                    }

                    free(relPath);
                    free(name);
                }                       /* "relPath" & "name" allocated */
            }
        }

        if (ENOENT == status)
            status = 0;

        beCloseCursor(&cursor);
    }                                   /* "cursor" allocated */

    if (status)
        log_add("Couldn't synchronize node \"%s\"", absPath);

    return status;
}

/*
 * Returns a binary value of a node.
 *
 * Arguments:
 *      node            Pointer to the node whose value is to be returned.
 *                      Shall not be NULL.
 *      name            Pointer to the name of the value.  Shall not be NULL.
 *      value           Pointer to the location to hold the binary value.
 *                      Shall not be NULL.
 *      typeStruct      Pointer to type-specific functions.  Shall not be NULL.
 * Returns:
 *      0               Success.  "*value" is set.
 *      ENOENT          No such value.  "log_start()" called.
 *      ENOMEM          System error.  "log_start()" called.
 */
static RegStatus getNodeValue(
    const RegNode* const        node,
    const char* const           name,
    void* const                 value,
    const TypeStruct* const     typeStruct)
{
    RegStatus   status = initRegistry(0);

    if (0 == status) {
        char*       string;

        if (0 == (status = rn_getValue(node, name, &string))) {
            status = typeStruct->parse(string, value);
            free(string);
        }                               /* "string" allocated */
    }

    return status;
}

/*
 * Returns the binary representation of a value from the registry.
 *
 * Preconditions:
 *      The registry has been initialized.
 * Arguments:
 *      path            Pointer to the absolute path name of the value to be
 *                      returned.  Shall not be NULL.
 *      value           Pointer to memory to hold the binary value.  Shall not
 *                      be NULL.
 *      typeStruct      Pointer to type-specific functions.  Shall not be NULL.
 * Returns:
 *      0               Success.  "*value" is not NULL.
 *      ENOENT          No value found for "path".  "log_start()" called.
 *      EINVAL          The path name isn't valid.  "log_start()" called.
 *      EILSEQ          The value found isn't the expected type.  "log_start()"
 *                      called.
 *      EIO             Backend database error.  "log_start()" called.
 *      ENOMEM          System error.  "log_start()" called.
 */
static RegStatus getValue(
    const char* const           path,
    void* const                 value,
    const TypeStruct* const     typeStruct)
{
    RegStatus   status = initRegistry(0);

    if (0 == status) {
        RegNode*    lastNode;
        char*       remPath;

        status = rn_getLastNode(_rootNode, path+1, &lastNode, &remPath);

        if (0 == status) {
            if (0 == *remPath) {
                log_start("\"%s\" is a node; not a value", path);
                status = ENOENT;
            }
            else if (0 == (status = flush(lastNode))) {
                if (0 == (status = sync(lastNode))) {
                    status = getNodeValue(lastNode, remPath, value, typeStruct);
                }
            }

            free(remPath);
        }                               /* "remPath" allocated */
    }

    if (0 != status && ENOENT != status)
        log_add("Couldn't get value \"%s\"", path);

    return status;
}

/*
 * Puts a value into a node.
 *
 * Arguments:
 *      node            Pointer to the node into which to put the value.  Shall
 *                      not be NULL.
 *      name            Pointer to the name of the value.  Shall not be NULL.
 *      value           Pointer to the value.  Shall not be NULL.
 *      typeStruct      Pointer to type-specific functions.  Shall not be NULL.
 * Returns:
 *      0               Success
 *      ENOMEM          System error.  "log_start()" called.
 *      EEXIST          The value would have the same absolute path name as an
 *                      existing node.  "log_start()" called.
 */
static RegStatus putNodeValue(
    RegNode* const              node,
    const char* const           name,
    const void* const           value,
    const TypeStruct* const     typeStruct)
{
    RegStatus   status = initRegistry(1);

    if (0 == status) {
        if (0 == (status = typeStruct->format(value, _formatBuf))) {
            ValueThing* vt;

            if (0 == (status = rn_putValue(node, name,
                    sb_string(_formatBuf), &vt))) {
                (void)vt_setStatus(vt, NOT_SYNCHED);
            }
        }
    }

    if (status)
        log_add("Couldn't put value \"%s\" in node \"%s\"", name,
            rn_getAbsPath(node));

    return status;
}

/*
 * Puts the string representation of a value into the registry.  Makes the
 * change persistent.
 *
 * Arguments:
 *      path            Pointer to the absolute path name under which to store
 *                      the value.  Shall not be NULL.
 *      value           Pointer to the value.  Shall not be NULL.
 *      typeStruct      Pointer to type-specific functions.  Shall not be NULL.
 * Returns:
 *      0               Success
 *      EINVAL          The absolute path name is invalid.  "log_start()"
 *                      called.
 *      ENOMEM          System error.  "log_start()" called.
 *      EIO             Backend database error.  "log_start()" called.
 *      EEXIST          A node or value would have to be created with the same
 *                      absolute path name as an existing value or node.
 *                      "log_start()" called.
 */
static RegStatus putValue(
    const char* const           path,
    const void* const           value,
    const TypeStruct* const     typeStruct)
{
    RegStatus   status = initRegistry(1);

    if (0 == status) {
        char*       nodePath;
        char*       valueName;

        if (0 == (status = reg_splitAbsPath(path, REG_SEP, &nodePath,
                &valueName))) {
            RegNode*    node;

            if (0 == (status = rn_ensure(_rootNode, nodePath, &node))) {
                if (0 == (status = putNodeValue(node, valueName, value,
                        typeStruct))) {
                    status = flush(node);
                }
            }

            free(valueName);
            free(nodePath);
        }                               /* "nodePath", "valueName" allocated */
    }

    return status;
}

/******************************************************************************
 * Public Functions:
 ******************************************************************************/

/*
 * Sets the pathname of the registry.  To have an effect, this function must be
 * called before any function that accesses the registry.
 *
 * Arguments:
 *      path            Pointer to the pathname of the registry.  May be NULL.
 *                      If NULL, then the default pathname is used.  The client
 *                      may free upon return.
 * Returns:
 *      0               Success
 *      ENOMEM          System error.  "log_start()" called.
 *      EPERM           Backend database already open.  "log_start()" called.
 */
RegStatus reg_setPathname(
    const char* const   path)
{
    RegStatus   status;

    if (NULL != _backend) {
        log_start("Can't set registry to \"%s\"; registry already open on "
            "\"%s\"", path, _registryPath);
        status = EPERM;
    }
    else {
        if (NULL == path) {
            _registryPath = REGISTRY_PATH;
            status = 0;
        }
        else {
            char*   clone;

            if (0 != (status = reg_cloneString(&clone, path))) {
                log_add("Couldn't set new registry pathname to \"%s\"", path);
            }
            else {
                if (REGISTRY_PATH != _registryPath)
                    free(_registryPath);

                _registryPath = clone;
            }
        }
    }

    return status;
}

/*
 * Closes the registry.  Frees all resources and unconditionally resets the
 * module (including the pathname of the registry).
 *
 * Returns:
 *      0               Success
 *      EIO             Backend database error.  "log_start()" called.
 */
RegStatus reg_close(void)
{
    RegStatus   status = closeRegistry();

    resetRegistry();

    return status;
}

/*
 * Resets the registry if it exists.  Unconditionally resets this module.
 *
 * Returns:
 *      0               Success
 *      EIO             Backend database error.  "log_start()" called.
 *      ENOMEM          System error.  "log_start()" called.
 */
RegStatus reg_reset(void)
{
    RegStatus   status;

    closeRegistry();

    status = beReset(_registryPath);

    resetRegistry();

    return status;
}

/*
 * Removes the registry if it exists.  Unconditionally resets this module.
 *
 * Returns:
 *      0               Success
 *      EIO             Backend database error.  "log_start()" called.
 *      ENOMEM          System error.  "log_start()" called.
 */
RegStatus reg_remove(void)
{
    RegStatus   status = initRegistry(1);

    if (0 == status) {
        closeRegistry();

        if (0 == status) {
            status = beRemove(_registryPath);
        }
    }

    resetRegistry();

    return status;
}

/*
 * Returns the string representation of a value from the registry.  The value
 * is obtained from the backing store.
 *
 * Arguments:
 *      path            Pointer to the absolute path name of the value to be
 *                      returned.  Shall not be NULL.
 *      value           Pointer to a pointer to the value.  Shall not be NULL.
 *                      Set upon successful return.  The client should call
 *                      "free(*value)" when the value is no longer needed.
 * Returns:
 *      0               Success.  Value found and put in "*value".
 *      ENOENT          No such value.  "log_start()" called.
 *      EINVAL          The path name isn't absolute.  "log_start()" called.
 *      EIO             Backend database error.  "log_start()" called.
 *      ENOMEM          System error.  "log_start()" called.
 */
RegStatus reg_getString(
    const char* const   path,
    char** const        value)
{
    return getValue(path, value, &stringStruct);
}

/*
 * Returns a value from the registry as an unsigned integer.
 *
 * Arguments:
 *      path            Pointer to the absolute path name of the value to be
 *                      returned.  Shall not be NULL.
 *      value           Pointer to memory to hold the value.  Shall not be
 *                      NULL.
 * Returns:
 *      0               Success.  Value found and put in "*value".
 *      ENOENT          No such value.  "log_start()" called.
 *      EINVAL          The path name isn't absolute.  "log_start()" called.
 *      EILSEQ          The value found isn't an unsigned integer.
 *                      "log_start()" called.
 *      EIO             Backend database error.  "log_start()" called.
 *      ENOMEM          System error.  "log_start()" called.
 */
RegStatus reg_getUint(
    const char* const   path,
    unsigned* const     value)
{
    return getValue(path, value,  &uintStruct);
}

/*
 * Returns a value from the registry as a time.
 *
 * Arguments:
 *      path            Pointer to the absolute path name of the value to be
 *                      returned.  Shall not be NULL.
 *      value           Pointer to memory to hold the value.  Shall not be NULL.
 *                      The client may free upon return.
 * Returns:
 *      0               Success.  Value found and put in "*value".
 *      ENOENT          No such value.  "log_start()" called.
 *      EINVAL          The path name isn't absolute.  "log_start()" called.
 *      EILSEQ          The value found isn't a timestamp.  "log_start()"
 *                      called.
 *      EIO             Backend database error.  "log_start()" called.
 *      ENOMEM          System error.  "log_start()" called.
 */
RegStatus reg_getTime(
    const char* const           path,
    timestampt* const           value)
{
    return getValue(path, value, &timeStruct);
}

/*
 * Returns a value from the registry as a signature.
 *
 * Arguments:
 *      path            Pointer to the absolute path name of the value to be
 *                      returned.  Shall not be NULL.
 *      value           Pointer to memory to hold the value.  Shall not be NULL.
 *                      The client may free upon return.
 * Returns:
 *      0               Success.  Value found and put in "*value".
 *      ENOENT          No such value.  "log_start()" called.
 *      EINVAL          The path name isn't absolute.  "log_start()" called.
 *      EILSEQ          The value found isn't a signature.  "log_start()"
 *                      called.
 *      EIO             Backend database error.  "log_start()" called.
 *      ENOMEM          System error.  "log_start()" called.
 */
RegStatus reg_getSignature(
    const char* const           path,
    signaturet* const           value)
{
    return getValue(path, value, &signatureStruct);
}

/*
 * Puts an unsigned integer value into the registry.
 *
 * Arguments:
 *      path            Pointer to the absolute path name of the value.  Shall
 *                      not be NULL.  Ancestral nodes are created if they
 *                      don't exist.
 *      value           The value to be written to the registry.
 * Returns:
 *      0               Success.
 *      EIO             Backend database error.  "log_start()" called.
 *      ENOMEM          System error.  "log_start()" called.
 *      EEXIST          A node or value would have to be created with the same
 *                      absolute path name as an existing value or node.
 *                      "log_start()" called.
 */
RegStatus reg_putUint(
    const char* const   path,
    const unsigned      value)
{
    return putValue(path, &value, &uintStruct);
}

/*
 * Puts a string value into the registry.
 *
 * Arguments:
 *      path            Pointer to the absolute path name of the value.  Shall
 *                      not be NULL.  Ancestral nodes are created if they
 *                      don't exist.
 *      value           Pointer to the value to be written to the registry.
 *                      Shall not be NULL.
 * Returns:
 *      0               Success.
 *      EIO             Backend database error.  "log_start()" called.
 *      ENOMEM          System error.  "log_start()" called.
 *      EEXIST          A node or value would have to be created with the same
 *                      absolute path name as an existing value or node.
 *                      "log_start()" called.
 */
RegStatus reg_putString(
    const char* const   path,
    const char* const   value)
{
    return putValue(path, value, &stringStruct);
}

/*
 * Puts a time value into the registry.
 *
 * Arguments:
 *      path            Pointer to the absolute path name of the value.  Shall
 *                      not be NULL.  Ancestral nodes are created if they
 *                      don't exist.
 *      value           Pointer to the value to be written to the registry.
 *                      Shall not be NULL.
 * Returns:
 *      0               Success.
 *      EIO             Backend database error.  "log_start()" called.
 *      ENOMEM          System error.  "log_start()" called.
 *      EEXIST          A node or value would have to be created with the same
 *                      absolute path name as an existing value or node.
 *                      "log_start()" called.
 */
RegStatus reg_putTime(
    const char* const           path,
    const timestampt* const     value)
{
    return putValue(path, value, &timeStruct);
}

/*
 * Puts a signature value into the registry.
 *
 * Arguments:
 *      path            Pointer to the absolute path name of the value.  Shall
 *                      not be NULL.  Ancestral nodes are created if they
 *                      don't exist.
 *      value           Pointer to the value to be written to the registry.
 *                      Shall not be NULL.
 * Returns:
 *      0               Success.
 *      EIO             Backend database error.  "log_start()" called.
 *      ENOMEM          System error.  "log_start()" called.
 *      EEXIST          A node or value would have to be created with the same
 *                      absolute path name as an existing value or node.
 *                      "log_start()" called.
 */
RegStatus reg_putSignature(
    const char* const   path,
    const signaturet    value)
{
    return putValue(path, value, &signatureStruct);
}

/*
 * Deletes a value from the registry.
 *
 * Arguments:
 *      path            Pointer to the absolute path name of the value to be
 *                      deleted.  Shall not be NULL.
 * Returns:
 *      0               Success
 *      EINVAL          The absolute path name is invalid.  "log_start()"
 *                      called.
 *      EIO             Backend database error.  "log_start()" called.
 *      ENOMEM          System error.  "log_start()" called.
 */
RegStatus reg_deleteValue(
    const char* const   path)
{
    RegStatus   status = initRegistry(1);

    if (0 == status) {
        char*   nodePath;
        char*   valueName;

        if (0 == (status = (reg_splitAbsPath(path, REG_SEP, &nodePath,
                &valueName)))) {
            RegNode*    node;

            if (ENOENT == (status = rn_find(_rootNode, nodePath,
                    &node))) {
                status = ENOENT;
            }
            else if (0 == status) {
                if (0 == (status = rn_deleteValue(node, valueName)))
                    status = flush(node);
            }

            free(valueName);
            free(nodePath);
        }                               /* "nodePath", "valueName" allocated */
    }

    if (status && ENOENT != status)
        log_add("Couldn't delete value \"%s\"", path);

    return status;
}

/*
 * Returns a node in the registry.  Can create the node and its ancestors if
 * they don't exist.  If the node didn't exist, then changes to the node won't
 * be made persistent until "reg_flush()" is called on the node or one of its
 * ancestors.
 *
 * Arguments:
 *      path            Pointer to the absolute path name of the node to be
 *                      returned.  Shall not be NULL.  The empty string obtains
 *                      the top-level node.
 *      node            Pointer to a pointer to a node.  Shall not be NULL.
 *                      Set on success.  The client should call
 *                      "regNode_free(*node)" when the node is no longer
 *                      needed.
 *      create          Whether or not to create the node if it doesn't
 *                      exist.  Zero means no; otherwise, yes.
 * Returns:
 *      0               Success.  "*node" is set.  The client should call
 *                      "regNode_free(*node)" when the node is no longer
 *                      needed.
 *      ENOENT          "create" was 0 and the node doesn't exist
 *      EINVAL          "path" isn't a valid absolute path name.  "log_start()
 *                      called.
 *      EIO             Backend database error.  "log_start()" called.
 *      ENOMEM          System error.  "log_start()" called.
 */
RegStatus reg_getNode(
    const char* const   path,
    RegNode** const     node,
    const int           create)
{
    RegStatus   status = reg_vetAbsPath(path);

    if (0 == status) {
        if (0 == (status = initRegistry(create))) {
            if (create) {
                status = rn_ensure(_rootNode, path+1, node);
            }
            else {
                RegNode*        lastNode;
                char*           remPath;

                if (0 == (status = rn_getLastNode(_rootNode, path+1, 
                        &lastNode, &remPath))) {
                    if (0 == *remPath) {
                        *node = lastNode;
                    }
                    else {
                        status = ENOENT;
                    }

                    free(remPath);
                }                       /* "remPath" allocated */
            }
        }
    }

    return status;
}

/*
 * Deletes a node and all of its children.  The node and its children are only
 * marked as being deleted: they are not removed from the registry until
 * "reg_flushNode()" is called on the node or one of its ancestors.
 *
 * Arguments:
 *      node            Pointer to the node to be deleted along with all it
 *                      children.  Shall not be NULL.
 */
void reg_deleteNode(
    RegNode*    node)
{
    rn_delete(node);
}

/*
 * Flushes all changes to a node and its children to the backend database.
 *
 * Arguments:
 *      node            Pointer to the node to be flushed to the registry.
 *                      Shall not be NULL.
 * Returns:
 *      0               Success
 *      EIO             Backend database error.  "log_start()" called.
 *      ENOMEM          System error.  "log_start()" called.
 */
RegStatus reg_flushNode(
    RegNode*    node)
{
    RegStatus   status = initRegistry(1);

    return (0 != status)
        ? status
        : flush(node);
}

/*
 * Returns the name of a node.
 *
 * Arguments:
 *      node            Pointer to the node whose name will be returned.  Shall
 *                      not be NULL.
 * Returns:
 *      Pointer to the name of the node.  The client shall not free.
 */
const char* reg_getNodeName(
    const RegNode* const        node)
{
    return rn_getName(node);
}

/*
 * Returns the absolute path name of a node.
 *
 * Arguments:
 *      node            Pointer to the node.  Shall not be NULL.
 * Returns:
 *      Pointer to the absolute path name of the node.  The client shall not
 *      free.
 */
const char* reg_getNodeAbsPath(
    const RegNode* const        node)
{
    return rn_getAbsPath(node);
}

/*
 * Adds a string value to a node.
 *
 * Arguments:
 *      node            Pointer to the node.  Shall not be NULL.
 *      name            Pointer to the name of the value.  Shall not be
 *                      NULL.  The client may free upon return.
 *      value           Pointer to the string value.  Shall not be NULL.  The
 *                      client may free upon return.
 * Returns:
 *      0               Success
 *      ENOMEM          System error.  "log_start()" called.
 *      EEXIST          The value would have the same absolute path name as an
 *                      existing node.  "log_start()" called.
 */
RegStatus reg_putNodeString(
    RegNode*            node,
    const char*         name,
    const char*         value)
{
    return putNodeValue(node, name, value, &stringStruct);
}

/*
 * Adds an unsigned integer value to a node.
 *
 * Arguments:
 *      node            Pointer to the parent node.  Shall not be NULL.
 *      name            Pointer to the name of the value-node.  Shall not be
 *                      NULL.  The client may free upon return.
 *      value           The unsigned integer value
 * Returns:
 *      0               Success
 *      ENOMEM          System error.  "log_start()" called.
 *      EEXIST          The value would have the same absolute path name as an
 *                      existing node.  "log_start()" called.
 */
RegStatus reg_putNodeUint(
    RegNode*            node,
    const char*         name,
    unsigned            value)
{
    return putNodeValue(node, name, &value, &uintStruct);
}

/*
 * Adds a time value to a node.
 *
 * Arguments:
 *      node            Pointer to the parent node.  Shall not be NULL.
 *      name            Pointer to the name of the value-node.  Shall not be
 *                      NULL.  The client may free upon return.
 *      value           Pointer to the time value.  Shall not be NULL.
 *                      The client may free upon return.
 * Returns:
 *      0               Success
 *      ENOMEM          System error.  "log_start()" called.
 *      EEXIST          The value would have the same absolute path name as an
 *                      existing node.  "log_start()" called.
 */
RegStatus reg_putNodeTime(
    RegNode*            node,
    const char*         name,
    const timestampt*   value)
{
    return putNodeValue(node, name, value, &timeStruct);
}

/*
 * Adds a signature value to a node.
 *
 * Arguments:
 *      node            Pointer to the node.  Shall not be NULL.
 *      name            Pointer to the name of the value.  Shall not be
 *                      NULL.  The client may free upon return.
 *      value           Pointer to the signature value.  Shall not be NULL.
 *                      The client may free upon return.
 * Returns:
 *      0               Success
 *      ENOMEM          System error.  "log_start()" called.
 *      EEXIST          The value would have the same absolute path name as an
 *                      existing node.  "log_start()" called.
 */
RegStatus reg_putNodeSignature(
    RegNode*            node,
    const char*         name,
    const signaturet    value)
{
    return putNodeValue(node, name, value, &signatureStruct);
}

/*
 * Returns a string value of a node.
 *
 * Arguments:
 *      node            Pointer to the node whose value is to be returned
 *                      as a string.  Shall not be NULL.
 *      name            Pointer to the name of the value.  Shall not be NULL.
 *      value           Pointer to a pointer to a string.  Shall not be NULL.
 *                      Set upon successful return.  The client should call call
 *                      "free(*value)" when the value is no longer needed.
 * Returns:
 *      0               Success.  "*value" is set.
 *      ENOENT          No such value
 *      ENOMEM          System error.  "log_start()" called.
 */
RegStatus reg_getNodeString(
    const RegNode* const        node,
    const char* const           name,
    char** const                value)
{
    return getNodeValue(node, name, value, &stringStruct);
}

/*
 * Returns an unsigned integer value of a node.
 *
 * Arguments:
 *      node            Pointer to the node whose value is to be returned.
 *                      Shall not be NULL.
 *      name            Pointer to the name of the value.  Shall not be NULL.
 *      value           Pointer to an unsigned integer.  Set upon success.
 *                      Shall not be NULL.
 * Returns:
 *      0               Success.  "*value" is set.
 *      ENOENT          No such value
 *      ENOMEM          System error.  "log_start()" called.
 *      EILSEQ          The value isn't an unsigned integer.  "log_start()"
 *                      called.
 */
RegStatus reg_getNodeUint(
    const RegNode* const        node,
    const char* const           name,
    unsigned* const             value)
{
    return getNodeValue(node, name, value, &uintStruct);
}

/*
 * Returns a time value of a node.
 *
 * Arguments:
 *      node            Pointer to the node whose value is to
 *                      be returned as a time.  Shall not be NULL.
 *      name            Pointer to the name of the value.  Shall not be NULL.
 *      value           Pointer to a time.  Set upon success.  Shall not be
 *                      NULL.  The client may free upon return.
 * Returns:
 *      0               Success.  "*value" is set.
 *      ENOENT          No such value
 *      ENOMEM          System error.  "log_start()" called.
 *      EILSEQ          The value isn't a time.  "log_start()" called.
 */
RegStatus reg_getNodeTime(
    const RegNode* const        node,
    const char* const           name,
    timestampt* const           value)
{
    return getNodeValue(node, name, value, &timeStruct);
}

/*
 * Returns a signature value of a node.
 *
 * Arguments:
 *      node            Pointer to the node whose value is to be returned as a
 *                      signature.  Shall not be NULL.
 *      name            Pointer to the name of the value.  Shall not be NULL.
 *      value           Pointer to a signature.  Set upon success.  Shall not
 *                      be NULL.  The client may free upon return.
 * Returns:
 *      0               Success.  "*value" is not NULL.
 *      ENOENT          No such value
 *      ENOMEM          System error.  "log_start()" called.
 *      EILSEQ          The value isn't a signature.  "log_start()" called.
 */
RegStatus reg_getNodeSignature(
    const RegNode* const        node,
    const char* const           name,
    signaturet* const           value)
{
    return getNodeValue(node, name, value, &signatureStruct);
}

/*
 * Deletes a value from a node.  The value is only marked as being deleted: it
 * is not removed from the registry until "reg_flushNode()" is called on the
 * node or one of its ancestors.
 *
 * Arguments:
 *      node            Pointer to the node to have the value deleted.  Shall
 *                      note be NULL.
 *      name            Pointer to the name of the value to be deleted.  Shall
 *                      not be NULL.
 * Returns:
 *      0               Success
 *      ENOENT          No such value
 *      ENOMEM          System error.  "log_start()" called.
 *      EPERM           The node has been deleted.  "log_start()" called.
 */
RegStatus reg_deleteNodeValue(
    RegNode* const      node,
    const char* const   name)
{
    RegStatus   status = initRegistry(1);

    if (0 == status)
        status = rn_deleteValue(node, name);

    return status;
}

/*
 * Visits a node and all its descendents in the natural order of their path
 * names.
 *
 * Arguments:
 *      node            Pointer to the node at which to start.  Shall not be
 *                      NULL.
 *      func            Pointer to the function to call at each node.  Shall
 *                      not be NULL.  The function shall not modify the set
 *                      of child-nodes to which the node being visited belongs.
 * Returns:
 *      0               Success
 *      else            The first non-zero value returned by "func".
 */
RegStatus reg_visitNodes(
    RegNode* const      node,
    const NodeFunc      func)
{
    return rn_visitNodes(node, func);
}

/*
 * Visits all the values of a node in the natural order of their path names.
 *
 * Arguments:
 *      node            Pointer to the node whose values are to be visited.
 *                      Shall not be NULL.
 *      func            Pointer to the function to call for each value.
 *                      Shall not be NULL.  The function shall not modify the
 *                      set of values to which the visited value belongs.
 * Returns:
 *      0               Success
 *      ENOMEM          System error.  "log_start()" called.
 *      else            The first non-zero value returned by "func"
 */
RegStatus reg_visitValues(
    RegNode* const      node,
    const ValueFunc     func)
{
    return rn_visitValues(node, func, NULL);
}
