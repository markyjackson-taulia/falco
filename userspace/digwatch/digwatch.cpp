#define __STDC_FORMAT_MACROS

#include <stdio.h>
#include <iostream>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <algorithm>
#include <unistd.h>
#include <getopt.h>

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

#include <sinsp.h>
#include <config_digwatch.h>
#include "rules.h"
#include "formats.h"
#include "fields.h"
#include "utils.h"

static bool g_terminate = false;

static void signal_callback(int signal)
{
	g_terminate = true;
}


std::vector<string> valid_output_names {"stdout", "syslog"};

//
// Program help
//
static void usage()
{
    printf(
	   "Usage: digwatch [options] rules_filename\n\n"
	   "Options:\n"
	   " -h, --help         Print this page\n"
	   " -m <filename>, --main-lua <filename>\n"
	   "                    Name of lua compiler main file\n"
	   "                    (default: rules_loader.lua)\n"
	   " -N                 Don't convert port numbers to names.\n"
	   " -o                 Output type (options are 'stdout', 'syslog', default is 'stdout')\n"
	   "                    process or into a script.\n"
	   "\n"
    );
}

string lua_on_event = "on_event";

//
// Event processing loop
//
void do_inspect(sinsp* inspector,
		digwatch_rules* rules,
		string output_name,
		lua_State* ls)
{
	int32_t res;
	sinsp_evt* ev;
	string line;

	//
	// Loop through the events
	//
	while(1)
	{

		if(g_terminate)
		{
			break;
		}

		res = inspector->next(&ev);

		if(res == SCAP_TIMEOUT)
		{
			continue;
		}
		else if(res == SCAP_EOF)
		{
			break;
		}
		else if(res != SCAP_SUCCESS)
		{
			//
			// Event read error.
			// Notify the chisels that we're exiting, and then die with an error.
			//
			cerr << "res = " << res << endl;
			throw sinsp_exception(inspector->getlasterr().c_str());
		}

		if(!inspector->is_debug_enabled() &&
			ev->get_category() & EC_INTERNAL)
		{
			continue;
		}

		lua_getglobal(ls, lua_on_event.c_str());

		if(lua_isfunction(ls, -1))
		{
			lua_pushlightuserdata(ls, ev);
			lua_pushnumber(ls, ev->get_check_id());
			lua_pushstring(ls, output_name.c_str());

			if(lua_pcall(ls, 3, 0, 0) != 0)
			{
				const char* lerr = lua_tostring(ls, -1);
				string err = "Error invoking function output: " + string(lerr);
				throw sinsp_exception(err);
			}
		}
		else
		{
			throw sinsp_exception("No function " + lua_on_event + " found in lua compiler module");
		}
	}
}

void add_lua_path(lua_State *ls, string path)
{
	string cpath = string(path);
	path += "?.lua";
	cpath += "?.so";

	lua_getglobal(ls, "package");

	lua_getfield(ls, -1, "path");
	string cur_path = lua_tostring(ls, -1 );
	cur_path += ';';
	lua_pop(ls, 1);

	cur_path.append(path.c_str());

	lua_pushstring(ls, cur_path.c_str());
	lua_setfield(ls, -2, "path");

	lua_getfield(ls, -1, "cpath");
	string cur_cpath = lua_tostring(ls, -1 );
	cur_cpath += ';';
	lua_pop(ls, 1);

	cur_cpath.append(cpath.c_str());

	lua_pushstring(ls, cur_cpath.c_str());
	lua_setfield(ls, -2, "cpath");

	lua_pop(ls, 1);
}

//
// ARGUMENT PARSING AND PROGRAM SETUP
//
int digwatch_init(int argc, char **argv)
{
	int result = EXIT_SUCCESS;
	sinsp* inspector = NULL;
	digwatch_rules* rules = NULL;
	int op;
	sinsp_evt::param_fmt event_buffer_format = sinsp_evt::PF_NORMAL;
	int long_index = 0;
	string lua_main_filename;
	string output_name = "stdout";
	string lua_dir = DIGWATCH_INSTALLATION_DIR;
	lua_State* ls = NULL;

	static struct option long_options[] =
	{
		{"help", no_argument, 0, 'h' },
		{"main-lua", required_argument, 0, 'u' },
		{0, 0, 0, 0}
	};

	try
	{
		inspector = new sinsp();
		bool valid;

		//
		// Parse the args
		//
		while((op = getopt_long(argc, argv,
                                        "hm:No:",
                                        long_options, &long_index)) != -1)
		{
			switch(op)
			{
			case 'h':
				usage();
				goto exit;
			case 'm':
				lua_main_filename = optarg;
				break;
			case 'N':
				inspector->set_hostname_and_port_resolution_mode(false);
				break;
			case 'o':
				valid = std::find(valid_output_names.begin(), valid_output_names.end(), optarg) != valid_output_names.end();
				if (!valid)
				{
					throw sinsp_exception(string("Invalid output name ") + optarg);
				}
				output_name = optarg;
				break;
			case '?':
				result = EXIT_FAILURE;
				goto exit;
			default:
				break;
			}

		}

		inspector->set_buffer_format(event_buffer_format);

		string rules_file;

		if(optind < argc)
		{
#ifdef HAS_FILTERING
			for(int32_t j = optind ; j < argc; j++)
			{
				rules_file += argv[j];
				if(j < argc - 1)
				{
					rules_file += " ";
				}
			}

#else
			fprintf(stderr, "filtering not compiled.\n");
			result = EXIT_FAILURE;
			goto exit;
#endif
		}

		if(rules_file.size() == 0) {
			usage();
			result = EXIT_FAILURE;
			goto exit;

		}

		if(signal(SIGINT, signal_callback) == SIG_ERR)
		{
			fprintf(stderr, "An error occurred while setting SIGINT signal handler.\n");
			result = EXIT_FAILURE;
			goto exit;
		}

		if(signal(SIGTERM, signal_callback) == SIG_ERR)
		{
			fprintf(stderr, "An error occurred while setting SIGTERM signal handler.\n");
			result = EXIT_FAILURE;
			goto exit;
		}

		//
		char* env_lua_dir = getenv("DIGWATCH_LUA_DIR");
		if(env_lua_dir)
		{
			lua_dir = string(env_lua_dir);
		}

		trim(lua_main_filename);
		if(lua_main_filename.size() == 0)
		{
			lua_main_filename = lua_dir + DIGWATCH_LUA_MAIN;
		}

		// Initialize Lua interpreter
		ls = lua_open();
		luaL_openlibs(ls);
		add_lua_path(ls, lua_dir);

		rules = new digwatch_rules(inspector, ls, lua_main_filename);

		digwatch_formats::init(inspector, ls);
		digwatch_fields::init(inspector, ls);

		digwatch_fields::init(inspector, ls);

		rules->load_rules(rules_file);
		inspector->set_filter(rules->get_filter());
		inspector->open("");

		do_inspect(inspector,
			   rules,
			   output_name,
			   ls);

		inspector->close();
	}
	catch(sinsp_exception& e)
	{
		cerr << e.what() << endl;
		result = EXIT_FAILURE;
	}
	catch(...)
	{
		printf("Exception\n");
		result = EXIT_FAILURE;
	}

exit:

	if(inspector)
	{
		delete inspector;
	}

	if(ls)
	{
		lua_close(ls);
	}
	return result;
}

//
// MAIN
//
int main(int argc, char **argv)
{
	return digwatch_init(argc, argv);
}