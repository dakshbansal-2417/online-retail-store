/*
 * protocol.h - constants for the ORS text protocol, shared by server and client.
 *
 * Request  : one line   COMMAND|arg1|arg2|...\n
 * Response : status line "OK <message>" or "ERR <message>"
 *            zero or more body lines
 *              "#Title"        -> starts a new section/table
 *              "a|b|c"         -> table row (first row of a section = header)
 *              ".xyz"          -> escaped row whose real text is "xyz"
 *            a line containing only "." terminates the response.
 */
#ifndef ORS_PROTOCOL_H
#define ORS_PROTOCOL_H

#define ORS_APP_NAME      "ORS - Online Retail Store"
#define ORS_VERSION       "1.0"
#define ORS_DEFAULT_PORT  8080
#define ORS_MAX_LINE      4096   /* longest request/response line          */
#define ORS_MAX_ARGS      16     /* max '|' separated fields in a request  */
#define ORS_SEP           '|'
#define ORS_END_LINE      "."

#endif /* ORS_PROTOCOL_H */
