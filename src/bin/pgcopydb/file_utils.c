/*
 * src/bin/pgcopydb/file_utils.c
 *   Implementations of utility functions for reading and writing files
 */

#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "postgres_fe.h"

#include "snprintf.h"

#include "cli_root.h"
#include "defaults.h"
#include "env_utils.h"
#include "file_utils.h"
#include "log.h"

static bool read_file_internal(FILE *fileStream,
							   const char *filePath,
							   char **contents,
							   long *fileSize);

/*
 * file_exists returns true if the given filename is known to exist
 * on the file system or false if it does not exist or in case of
 * error.
 */
bool
file_exists(const char *filename)
{
	bool exists = access(filename, F_OK) != -1;
	if (!exists && errno != 0)
	{
		/*
		 * Only log "interesting" errors here.
		 *
		 * The fact that the file does not exist is not interesting: we're
		 * retuning false and the caller figures it out, maybe then creating
		 * the file.
		 */
		if (errno != ENOENT && errno != ENOTDIR)
		{
			log_error("Failed to check if file \"%s\" exists: %m", filename);
		}
		return false;
	}

	return exists;
}


/*
 * file_is_empty returns true if the given filename is known to exist on the
 * file system and is empty: its content is "".
 */
bool
file_is_empty(const char *filename)
{
	if (file_exists(filename))
	{
		char *fileContents;
		long fileSize;

		if (!read_file(filename, &fileContents, &fileSize))
		{
			/* errors are logged */
			return false;
		}

		if (fileSize == 0)
		{
			return true;
		}
	}

	return false;
}


/*
 * directory_exists returns whether the given path is the name of a directory that
 * exists on the file system or not.
 */
bool
directory_exists(const char *path)
{
	struct stat info;

	if (!file_exists(path))
	{
		return false;
	}

	if (stat(path, &info) != 0)
	{
		log_error("Failed to stat \"%s\": %m\n", path);
		return false;
	}

	bool result = (info.st_mode & S_IFMT) == S_IFDIR;
	return result;
}


/*
 * ensure_empty_dir ensures that the given path points to an empty directory with
 * the given mode. If it fails to do so, it returns false.
 */
bool
ensure_empty_dir(const char *dirname, int mode)
{
	/* pg_mkdir_p might modify its input, so create a copy of dirname. */
	char dirname_copy[MAXPGPATH];
	strlcpy(dirname_copy, dirname, MAXPGPATH);

	if (directory_exists(dirname))
	{
		if (!rmtree(dirname, true))
		{
			log_error("Failed to remove directory \"%s\": %m", dirname);
			return false;
		}
	}
	else
	{
		/*
		 * reset errno, we don't care anymore that it failed because dirname
		 * doesn't exists.
		 */
		errno = 0;
	}

	if (pg_mkdir_p(dirname_copy, mode) == -1)
	{
		log_error("Failed to ensure empty directory \"%s\": %m", dirname);
		return false;
	}

	return true;
}


/*
 * fopen_with_umask is a version of fopen that gives more control. The main
 * advantage of it is that it allows specifying a umask of the file. This makes
 * sure files are not accidentally created with umask 777 if the user has it
 * configured in a weird way.
 *
 * This function returns NULL when opening the file fails. So this should be
 * handled. It will log an error in this case though, so that's not necessary
 * at the callsite.
 */
FILE *
fopen_with_umask(const char *filePath, const char *modes, int flags, mode_t umask)
{
	int fileDescriptor = open(filePath, flags, umask);
	if (fileDescriptor == -1)
	{
		log_error("Failed to open file \"%s\": %m", filePath);
		return NULL;
	}

	FILE *fileStream = fdopen(fileDescriptor, modes);
	if (fileStream == NULL)
	{
		log_error("Failed to open file \"%s\": %m", filePath);
		close(fileDescriptor);
	}
	return fileStream;
}


/*
 * fopen_read_only opens the file as a read only stream.
 */
FILE *
fopen_read_only(const char *filePath)
{
	/*
	 * Explanation of IGNORE-BANNED
	 * fopen is safe here because we open the file in read only mode. So no
	 * exclusive access is needed.
	 */
	return fopen(filePath, "rb"); /* IGNORE-BANNED */
}


/*
 * write_file writes the given data to the file given by filePath using
 * our logging library to report errors. If succesful, the function returns
 * true.
 */
bool
write_file(char *data, long fileSize, const char *filePath)
{
	FILE *fileStream = fopen_with_umask(filePath, "wb", FOPEN_FLAGS_W, 0644);

	if (fileStream == NULL)
	{
		/* errors have already been logged */
		return false;
	}

	if (fwrite(data, sizeof(char), fileSize, fileStream) < fileSize)
	{
		log_error("Failed to write file \"%s\": %m", filePath);
		fclose(fileStream);
		return false;
	}

	if (fclose(fileStream) == EOF)
	{
		log_error("Failed to write file \"%s\"", filePath);
		return false;
	}

	return true;
}


/*
 * append_to_file writes the given data to the end of the file given by
 * filePath using our logging library to report errors. If succesful, the
 * function returns true.
 */
bool
append_to_file(char *data, long fileSize, const char *filePath)
{
	FILE *fileStream = fopen_with_umask(filePath, "ab", FOPEN_FLAGS_A, 0644);

	if (fileStream == NULL)
	{
		/* errors have already been logged */
		return false;
	}

	if (fwrite(data, sizeof(char), fileSize, fileStream) < fileSize)
	{
		log_error("Failed to write file \"%s\": %m", filePath);
		fclose(fileStream);
		return false;
	}

	if (fclose(fileStream) == EOF)
	{
		log_error("Failed to write file \"%s\"", filePath);
		return false;
	}

	return true;
}


/*
 * read_file_if_exists is a utility function that reads the contents of a file
 * using our logging library to report errors. ENOENT is not considered worth
 * of a log message in this function, and we still return false in that case.
 *
 * If successful, the function returns true and fileSize points to the number
 * of bytes that were read and contents points to a buffer containing the entire
 * contents of the file. This buffer should be freed by the caller.
 */
bool
read_file_if_exists(const char *filePath, char **contents, long *fileSize)
{
	/* open a file */
	FILE *fileStream = fopen_read_only(filePath);

	if (fileStream == NULL)
	{
		if (errno != ENOENT)
		{
			log_error("Failed to open file \"%s\": %m", filePath);
		}
		return false;
	}

	return read_file_internal(fileStream, filePath, contents, fileSize);
}


/*
 * read_file is a utility function that reads the contents of a file using our
 * logging library to report errors.
 *
 * If successful, the function returns true and fileSize points to the number
 * of bytes that were read and contents points to a buffer containing the entire
 * contents of the file. This buffer should be freed by the caller.
 */
bool
read_file(const char *filePath, char **contents, long *fileSize)
{
	/* open a file */
	FILE *fileStream = fopen_read_only(filePath);
	if (fileStream == NULL)
	{
		log_error("Failed to open file \"%s\": %m", filePath);
		return false;
	}

	return read_file_internal(fileStream, filePath, contents, fileSize);
}


/*
 * read_file_internal is shared by both read_file and read_file_if_exists
 * functions.
 */
static bool
read_file_internal(FILE *fileStream,
				   const char *filePath, char **contents, long *fileSize)
{
	/* get the file size */
	if (fseek(fileStream, 0, SEEK_END) != 0)
	{
		log_error("Failed to read file \"%s\": %m", filePath);
		fclose(fileStream);
		return false;
	}

	*fileSize = ftell(fileStream);
	if (*fileSize < 0)
	{
		log_error("Failed to read file \"%s\": %m", filePath);
		fclose(fileStream);
		return false;
	}

	if (fseek(fileStream, 0, SEEK_SET) != 0)
	{
		log_error("Failed to read file \"%s\": %m", filePath);
		fclose(fileStream);
		return false;
	}

	/* read the contents */
	char *data = malloc(*fileSize + 1);
	if (data == NULL)
	{
		log_error("Failed to allocate %ld bytes", *fileSize);
		log_error(ALLOCATION_FAILED_ERROR);
		fclose(fileStream);
		return false;
	}

	if (fread(data, sizeof(char), *fileSize, fileStream) < *fileSize)
	{
		log_error("Failed to read file \"%s\": %m", filePath);
		fclose(fileStream);
		free(data);
		return false;
	}

	if (fclose(fileStream) == EOF)
	{
		log_error("Failed to read file \"%s\"", filePath);
		free(data);
		return false;
	}

	data[*fileSize] = '\0';
	*contents = data;

	return true;
}


/*
 * move_file is a utility function to move a file from sourcePath to
 * destinationPath. It behaves like mv system command. First attempts to move
 * a file using rename. if it fails with EXDEV error, the function duplicates
 * the source file with owner and permission information and removes it.
 */
bool
move_file(char *sourcePath, char *destinationPath)
{
	if (strncmp(sourcePath, destinationPath, MAXPGPATH) == 0)
	{
		/* nothing to do */
		log_warn("Source and destination are the same \"%s\", nothing to move.",
				 sourcePath);
		return true;
	}

	if (!file_exists(sourcePath))
	{
		log_error("Failed to move file, source file \"%s\" does not exist.",
				  sourcePath);
		return false;
	}

	if (file_exists(destinationPath))
	{
		log_error("Failed to move file, destination file \"%s\" already exists.",
				  destinationPath);
		return false;
	}

	/* first try atomic move operation */
	if (rename(sourcePath, destinationPath) == 0)
	{
		return true;
	}

	/*
	 * rename fails with errno = EXDEV when moving file to a different file
	 * system.
	 */
	if (errno != EXDEV)
	{
		log_error("Failed to move file \"%s\" to \"%s\": %m",
				  sourcePath, destinationPath);
		return false;
	}

	if (!duplicate_file(sourcePath, destinationPath))
	{
		/* specific error is already logged */
		log_error("Canceling file move due to errors.");
		return false;
	}

	/* everything is successful we can remove the file */
	unlink_file(sourcePath);

	return true;
}


/*
 * duplicate_file is a utility function to duplicate a file from sourcePath to
 * destinationPath. It reads the contents of the source file and writes to the
 * destination file. It expects non-existing destination file and does not
 * copy over if it exists. The function returns true on successful execution.
 *
 * Note: the function reads the whole file into memory before copying out.
 */
bool
duplicate_file(char *sourcePath, char *destinationPath)
{
	char *fileContents;
	long fileSize;
	struct stat sourceFileStat;

	if (!read_file(sourcePath, &fileContents, &fileSize))
	{
		/* errors are logged */
		return false;
	}

	if (file_exists(destinationPath))
	{
		log_error("Failed to duplicate, destination file already exists : %s",
				  destinationPath);
		return false;
	}

	bool foundError = !write_file(fileContents, fileSize, destinationPath);

	free(fileContents);

	if (foundError)
	{
		/* errors are logged in write_file */
		return false;
	}

	/* set uid gid and mode */
	if (stat(sourcePath, &sourceFileStat) != 0)
	{
		log_error("Failed to get ownership and file permissions on \"%s\"",
				  sourcePath);
		foundError = true;
	}
	else
	{
		if (chown(destinationPath, sourceFileStat.st_uid, sourceFileStat.st_gid) != 0)
		{
			log_error("Failed to set user and group id on \"%s\"",
					  destinationPath);
			foundError = true;
		}
		if (chmod(destinationPath, sourceFileStat.st_mode) != 0)
		{
			log_error("Failed to set file permissions on \"%s\"",
					  destinationPath);
			foundError = true;
		}
	}

	if (foundError)
	{
		/* errors are already logged */
		unlink_file(destinationPath);
		return false;
	}

	return true;
}


/*
 * create_symbolic_link creates a symbolic link to source path.
 */
bool
create_symbolic_link(char *sourcePath, char *targetPath)
{
	if (symlink(sourcePath, targetPath) != 0)
	{
		log_error("Failed to create symbolic link to \"%s\": %m", targetPath);
		return false;
	}
	return true;
}


/*
 * path_in_same_directory constructs the path for a file with name fileName
 * that is in the same directory as basePath, which should be an absolute
 * path. The result is written to destinationPath, which should be at least
 * MAXPATH in size.
 */
void
path_in_same_directory(const char *basePath, const char *fileName,
					   char *destinationPath)
{
	strlcpy(destinationPath, basePath, MAXPGPATH);
	get_parent_directory(destinationPath);
	join_path_components(destinationPath, destinationPath, fileName);
}


/* From PostgreSQL sources at src/port/path.c */
#ifndef WIN32
#define IS_PATH_VAR_SEP(ch) ((ch) == ':')
#else
#define IS_PATH_VAR_SEP(ch) ((ch) == ';')
#endif


/*
 * search_path_first copies the first entry found in PATH to result. result
 * should be a buffer of (at least) MAXPGPATH size.
 * The function returns false and logs an error when it cannot find the command
 * in PATH.
 */
bool
search_path_first(const char *filename, char *result, int logLevel)
{
	SearchPath paths = { 0 };

	if (!search_path(filename, &paths) || paths.found == 0)
	{
		log_level(logLevel, "Failed to find %s command in your PATH", filename);
		return false;
	}

	strlcpy(result, paths.matches[0], MAXPGPATH);

	return true;
}


/*
 * Searches all the directories in the PATH environment variable for the given
 * filename. Returns number of occurrences and each match found with its
 * fullname, including the given filename, in the given pre-allocated
 * SearchPath result.
 */
bool
search_path(const char *filename, SearchPath *result)
{
	char pathlist[MAXPATHSIZE] = { 0 };

	/* we didn't count nor find anything yet */
	result->found = 0;

	/* Create a copy of pathlist, because we modify it here. */
	if (!get_env_copy("PATH", pathlist, sizeof(pathlist)))
	{
		/* errors have already been logged */
		return false;
	}

	char *path = pathlist;

	while (path != NULL)
	{
		char candidate[MAXPGPATH] = { 0 };
		char *sep = first_path_var_separator(path);

		/* split path on current token, null-terminating string at separator */
		if (sep != NULL)
		{
			*sep = '\0';
		}

		(void) join_path_components(candidate, path, filename);
		(void) canonicalize_path(candidate);

		if (file_exists(candidate))
		{
			strlcpy(result->matches[result->found++], candidate, MAXPGPATH);
		}

		path = (sep == NULL ? NULL : sep + 1);
	}

	return true;
}


/*
 * search_path_deduplicate_symlinks traverse the SearchPath result obtained by
 * calling the search_path() function and removes entries that are pointing to
 * the same binary on-disk.
 *
 * In modern debian installations, for instance, we have /bin -> /usr/bin; and
 * then we might find pg_config both in /bin/pg_config and /usr/bin/pg_config
 * although it's only been installed once, and both are the same file.
 *
 * We use realpath() to deduplicate entries, and keep the entry that is not a
 * symbolic link.
 */
bool
search_path_deduplicate_symlinks(SearchPath *results, SearchPath *dedup)
{
	/* now re-initialize the target structure dedup */
	dedup->found = 0;

	for (int rIndex = 0; rIndex < results->found; rIndex++)
	{
		bool alreadyThere = false;

		char *currentPath = results->matches[rIndex];
		char currentRealPath[PATH_MAX] = { 0 };

		if (realpath(currentPath, currentRealPath) == NULL)
		{
			log_error("Failed to normalize file name \"%s\": %m", currentPath);
			return false;
		}

		/* add-in the realpath to dedup, unless it's already in there */
		for (int dIndex = 0; dIndex < dedup->found; dIndex++)
		{
			if (strcmp(dedup->matches[dIndex], currentRealPath) == 0)
			{
				alreadyThere = true;

				log_debug("dedup: skipping \"%s\"", currentPath);
				break;
			}
		}

		if (!alreadyThere)
		{
			int bytesWritten =
				strlcpy(dedup->matches[dedup->found++],
						currentRealPath,
						MAXPGPATH);

			if (bytesWritten >= MAXPGPATH)
			{
				log_error(
					"Real path \"%s\" is %d bytes long, and pgcopydb "
					"is limited to handling paths of %d bytes long, maximum",
					currentRealPath,
					(int) strlen(currentRealPath),
					MAXPGPATH);

				return false;
			}
		}
	}

	return true;
}


/*
 * unlink_state_file calls unlink(2) on the state file to make sure we don't
 * leave a lingering state on-disk.
 */
bool
unlink_file(const char *filename)
{
	if (unlink(filename) == -1)
	{
		/* if it didn't exist yet, good news! */
		if (errno != ENOENT && errno != ENOTDIR)
		{
			log_error("Failed to remove file \"%s\": %m", filename);
			return false;
		}
	}

	return true;
}


/*
 * get_program_absolute_path returns the absolute path of the current program
 * being executed. Note: the shell is responsible to set that in interactive
 * environments, and when the pgcopydb binary is in the PATH of the user,
 * then argv[0] (here pgcopydb_argv0) is just "pgcopydb".
 */
bool
set_program_absolute_path(char *program, int size)
{
#if defined(__APPLE__)
	int actualSize = _NSGetExecutablePath(program, (uint32_t *) &size);

	if (actualSize != 0)
	{
		log_error("Failed to get absolute path for the pgcopydb program, "
				  "absolute path requires %d bytes and we support paths up "
				  "to %d bytes only", actualSize, size);
		return false;
	}

	log_debug("Found absolute program: \"%s\"", program);

#else

	/*
	 * On Linux and FreeBSD and Solaris, we can find a symbolic link to our
	 * program and get the information with readlink. Of course the /proc entry
	 * to read is not the same on both systems, so we try several things here.
	 */
	bool found = false;
	char *procEntryCandidates[] = {
		"/proc/self/exe",       /* Linux */
		"/proc/curproc/file",   /* FreeBSD */
		"/proc/self/path/a.out" /* Solaris */
	};
	int procEntrySize = sizeof(procEntryCandidates) / sizeof(char *);
	int procEntryIndex = 0;

	for (procEntryIndex = 0; procEntryIndex < procEntrySize; procEntryIndex++)
	{
		if (readlink(procEntryCandidates[procEntryIndex], program, size) != -1)
		{
			found = true;
			log_debug("Found absolute program \"%s\" in \"%s\"",
					  program,
					  procEntryCandidates[procEntryIndex]);
		}
		else
		{
			/* when the file does not exist, we try our next guess */
			if (errno != ENOENT && errno != ENOTDIR)
			{
				log_error("Failed to get absolute path for the "
						  "pgcopydb program: %m");
				return false;
			}
		}
	}

	if (found)
	{
		return true;
	}
	else
	{
		/*
		 * Now either return pgcopydb_argv0 when that's an absolute filename,
		 * or search for it in the PATH otherwise.
		 */
		SearchPath paths = { 0 };

		if (pgcopydb_argv0[0] == '/')
		{
			strlcpy(program, pgcopydb_argv0, size);
			return true;
		}

		if (!search_path(pgcopydb_argv0, &paths) || paths.found == 0)
		{
			log_error("Failed to find \"%s\" in PATH environment",
					  pgcopydb_argv0);
			exit(EXIT_CODE_INTERNAL_ERROR);
		}
		else
		{
			log_debug("Found \"%s\" in PATH at \"%s\"",
					  pgcopydb_argv0, paths.matches[0]);
			strlcpy(program, paths.matches[0], size);

			return true;
		}
	}
#endif

	return true;
}


/*
 * normalize_filename returns the real path of a given filename that belongs to
 * an existing file on-disk, resolving symlinks and pruning double-slashes and
 * other weird constructs. filename and dst are allowed to point to the same
 * adress.
 */
bool
normalize_filename(const char *filename, char *dst, int size)
{
	/* normalize the path to the configuration file, if it exists */
	if (file_exists(filename))
	{
		char realPath[PATH_MAX] = { 0 };

		if (realpath(filename, realPath) == NULL)
		{
			log_fatal("Failed to normalize file name \"%s\": %m", filename);
			return false;
		}

		if (strlcpy(dst, realPath, size) >= size)
		{
			log_fatal("Real path \"%s\" is %d bytes long, and pgcopydb "
					  "is limited to handling paths of %d bytes long, maximum",
					  realPath, (int) strlen(realPath), size);
			return false;
		}
	}
	else
	{
		char realPath[PATH_MAX] = { 0 };

		/* protect against undefined behavior if dst overlaps with filename */
		strlcpy(realPath, filename, MAXPGPATH);
		strlcpy(dst, realPath, MAXPGPATH);
	}

	return true;
}


/*
 * fformat is a secured down version of pg_fprintf:
 *
 * Additional security checks are:
 *  - make sure stream is not null
 *  - make sure fmt is not null
 *  - rely on pg_fprintf Assert() that %s arguments are not null
 */
int
fformat(FILE *stream, const char *fmt, ...)
{
	va_list args;

	if (stream == NULL || fmt == NULL)
	{
		log_error("BUG: fformat is called with a NULL target or format string");
		return -1;
	}

	va_start(args, fmt);
	int len = pg_vfprintf(stream, fmt, args);
	va_end(args);
	return len;
}


/*
 * sformat is a secured down version of pg_snprintf
 */
int
sformat(char *str, size_t count, const char *fmt, ...)
{
	va_list args;

	if (str == NULL || fmt == NULL)
	{
		log_error("BUG: sformat is called with a NULL target or format string");
		return -1;
	}

	va_start(args, fmt);
	int len = pg_vsnprintf(str, count, fmt, args);
	va_end(args);

	if (len >= count)
	{
		log_error("BUG: sformat needs %d bytes to expend format string \"%s\", "
				  "and a target string of %lu bytes only has been given.",
				  len, fmt,
				  (unsigned long) count);
	}

	return len;
}


/*
 * set_ps_title sets the process title seen in ps/top and friends, truncating
 * if there is not enough space, rather than causing memory corruption.
 *
 * Inspired / stolen from Postgres code src/backend/utils/misc/ps_status.c with
 * most of the portability bits removed. At the moment we prefer simple code
 * that works on few targets to highly portable code.
 */
void
init_ps_buffer(int argc, char **argv)
{
#if defined(__linux__) || defined(__darwin__)
	char *end_of_area = NULL;
	int i;

	/*
	 * check for contiguous argv strings
	 */
	for (i = 0; i < argc; i++)
	{
		if (i == 0 || end_of_area + 1 == argv[i])
		{
			end_of_area = argv[i] + strlen(argv[i]); /* lgtm[cpp/tainted-arithmetic] */
		}
	}

	if (end_of_area == NULL)    /* probably can't happen? */
	{
		ps_buffer = NULL;
		ps_buffer_size = 0;
		return;
	}

	ps_buffer = argv[0];
	last_status_len = ps_buffer_size = end_of_area - argv[0]; /* lgtm[cpp/tainted-arithmetic] */

#else
	ps_buffer = NULL;
	ps_buffer_size = 0;

	return;
#endif
}


/*
 * set_ps_title sets our process name visible in ps/top/pstree etc.
 */
void
set_ps_title(const char *title)
{
	if (ps_buffer == NULL)
	{
		/* noop */
		return;
	}

	int n = sformat(ps_buffer, ps_buffer_size, "%s", title);

	/* pad our process title string */
	for (size_t i = n; i < ps_buffer_size; i++)
	{
		*(ps_buffer + i) = '\0';
	}
}
