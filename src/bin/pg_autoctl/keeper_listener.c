/*
 * src/bin/pg_autoctl/keeper_listener.c
 *	 The keeper listener is a process that reads commands from a PIPE and then
 *	 synchronoulsy writes to the same PIPE the result of running given
 *	 commands. This process is used to implement FSM transitions when running
 *	 in --disable-monitor mode.
 *
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the PostgreSQL License.
 *
 *   One reason to do use a separate process with a PIPE to enable two-way sync
 *   communication is that we don't want the postgres processes to inherit from
 *   the HTTPd server socket and other pg_autoctl context; so the clean way is
 *   to have a process hierarchy where the HTTPd service is not the parent of
 *   the Postgres related activity.
 *
 *   pg_autoctl run
 *    - keeper run loop   [monitor enabled]
 *    - httpd server      [all cases]
 *    - listener          [all cases] [published API varies]
 *      - pg_autoctl do fsm assign single
 *      - pg_autoctl do fsm assign wait_primary
 *      - pg_autoctl enable maintenance
 *      - pg_autoctl disable maintenance
 *    - postgres -p 5432 -h localhost -k /tmp
 *
 *   We still want the `postgres` process to run as a chile of the main
 *   pg_autoctl service. When PostgreSQL is started by a listener command, this
 *   means we also need a communication/execution channel with the pg_autoctl
 *   parent process.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "postgres_fe.h"
#include "pqexpbuffer.h"

#include "cli_root.h"
#include "defaults.h"
#include "fsm.h"
#include "keeper.h"
#include "keeper_listener.h"
#include "log.h"
#include "signals.h"
#include "state.h"

CommandPipe listenerCommandPipe = { 0 };

static bool keeper_listener_read_commands(int cmdIn, int resOut);


/*
 * keeper_listener_start starts a subprocess that listens on a given PIPE for
 * commands to run. The commands it implements are the PG_AUTOCTL_DEBUG=1
 * commands.
 */
bool
keeper_listener_start(const char *pgdata, pid_t *listenerPid)
{
	pid_t pid;
	int log_level = logLevel;

	/* Flush stdio channels just before fork, to avoid double-output problems */
	fflush(stdout);
	fflush(stderr);

	/* create the communication pipe to the listener */
	if (pipe(listenerCommandPipe.cmdPipe) < 0)
	{
		log_error("Failed to create a pipe with the listener process: %s",
				  strerror(errno));
		return false;
	}

	if (pipe(listenerCommandPipe.resPipe) < 0)
	{
		log_error("Failed to create a pipe with the listener process: %s",
				  strerror(errno));
		return false;
	}

	/* time to create the listener sub-process, that receives the commands */
	pid = fork();

	switch (pid)
	{
		case -1:
		{
			log_error("Failed to fork the listener process");
			return false;
		}

		case 0:
		{
			/* fork succeeded, in child */
			close(listenerCommandPipe.cmdPipe[1]);
			close(listenerCommandPipe.resPipe[0]);

			/* we execute commands through the pg_autoctl do command line */
			setenv(PG_AUTOCTL_DEBUG, "1", 1);

			return keeper_listener_read_commands(
				listenerCommandPipe.cmdPipe[0],
				listenerCommandPipe.resPipe[1]);
		}

		default:
		{
			/* fork succeeded, in parent */
			close(listenerCommandPipe.cmdPipe[0]);
			close(listenerCommandPipe.resPipe[1]);

			log_debug("pg_autoctl listener started in subprocess %d", pid);
			*listenerPid = pid;
			return true;
		}
	}
}


/*
 * keeper_listener_read_commands reads from the listener PIPE for commands to
 * execute. Commands are expected to be in the form of
 *
 *  fsm assign single
 *
 * And then the listener executes the following command:
 *
 *   pg_autoctl do fsm assign single
 */
static bool
keeper_listener_read_commands(int cmdIn, int resOut)
{
	char buffer[BUFSIZE] = { 0 };
	char *command = NULL;

	FILE *in = fdopen(cmdIn, "r");
	FILE *out = fdopen(resOut, "w");

	if (in == NULL || out == NULL)
	{
		log_error("Failed to open a file pointer for command pipe: %s",
				  strerror(errno));
		return false;
	}

	log_debug("Keeper listener started");

	while ((command = fgets(buffer, BUFSIZE, in)) != NULL)
	{
		/* split the command string in CLI arguments array */
		int argc = 0;
		char *argv[12] = { 0 };
		char *ptr = command, *previous = command;

		int returnCode;
		char logs[BUFSIZE] = { 0 };
		char result[BUFSIZE] = { 0 };

		/* argv[0] should be our current running program */
		argv[argc++] = pg_autoctl_argv0;

		for (; *ptr != '\0'; ptr++)
		{
			if (*ptr == ' ' || *ptr == '\n')
			{
				char *next = ptr+1;

				*ptr = '\0';
				argv[argc++] = previous;

				/* prepare next round */
				previous = next;
			}
		}

		/* run the subcommand in a subprogram */
		if (!pg_autoctl_run_subcommand(argc, argv, &returnCode,
									   result, BUFSIZE,
									   logs, BUFSIZE))
		{
			/* FIXME: better error message is needed */
			log_error("Failed to run subcommand, returned %d", returnCode);
			if (strlen(logs) > 0)
			{
				log_error("%s", logs);
			}
		}

		fputs("output\n", out);
		fputs(result, out);
		fputs("\nlogs\n", out);
		fputs(logs, out);
		fputs("\nready\n", out);
		fflush(out);
	}

	return true;
}


typedef enum
{
	READING_UNKNOWN = 0,
	READING_OUTPUT,
	READING_LOGS,
	READING_DONE
} OutputReaderState;

/*
 * keeper_listener_send_command send a command to our command pipe, and waits
 * until we receive its whole result.
 */
bool
keeper_listener_send_command(const char *command, char *output, int size)
{
	char line[BUFSIZE] = { 0 };
	char *ptr = NULL;

	/* we're commandStream the parent process */
	FILE *commandStream = fdopen(listenerCommandPipe.cmdPipe[1], "w");
	FILE *resultStream = fdopen(listenerCommandPipe.resPipe[0], "r");

	OutputReaderState outputState = READING_UNKNOWN;
	PQExpBuffer commandOutput = NULL;
	PQExpBuffer commandLogs = NULL;

	if (commandStream == NULL || resultStream == NULL)
	{
		log_error("Failed to open a file for the listener's command pipe: %s",
				  strerror(errno));
		return false;
	}

	log_debug("keeper_listener_send_command: sending %s", command);

	if (fputs(command, commandStream) == EOF
		|| fputs("\n", commandStream) == EOF
		|| fflush(commandStream) == EOF)
	{
		log_error("Failed to send the command to the listener: %s",
				  strerror(errno));
		return false;
	}

	commandOutput = createPQExpBuffer();
	if (commandOutput == NULL)
	{
		log_error("Failed to allocate memory");
		return false;
	}

	commandLogs = createPQExpBuffer();
	if (commandLogs == NULL)
	{
		log_error("Failed to allocate memory");
		return false;
	}

	while (outputState != READING_DONE
		   && (ptr = fgets(line, BUFSIZE, resultStream)) != NULL)
	{
		if (outputState == READING_UNKNOWN && strcmp(line, "output\n") == 0)
		{
			outputState = READING_OUTPUT;
			continue;
		}

		if (outputState == READING_OUTPUT && strcmp(line, "logs\n") == 0)
		{
			outputState = READING_LOGS;
			continue;
		}

		if (outputState == READING_LOGS && strcmp(line, "ready\n") == 0)
		{
			outputState = READING_DONE;
			continue;
		}

		switch (outputState)
		{
			case READING_OUTPUT:
				appendPQExpBufferStr(commandOutput, line);
				break;

			case READING_LOGS:
				appendPQExpBufferStr(commandLogs, line);
				break;

			default:
				/* must be garbage, not interested into that */
				break;
		}
	}

	/* copy logs and results to the caller's allocated memory */
	strlcpy(output, commandOutput->data, size);

	/* free dynamically allocated memory */
	destroyPQExpBuffer(commandOutput);
	destroyPQExpBuffer(commandLogs);

	return true;
}