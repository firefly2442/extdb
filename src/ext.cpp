/*
Copyright (C) 2014 Declan Ireland <http://github.com/torndeco/extDB>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "ext.h"

#include <Poco/Data/Session.h>
#include <Poco/Data/SessionPool.h>

#include <Poco/Data/MySQL/Connector.h>
#include <Poco/Data/MySQL/MySQLException.h>
#include <Poco/Data/SQLite/Connector.h>
#include <Poco/Data/SQLite/SQLiteException.h>
#include "Poco/Data/ODBC/Connector.h"
#include "Poco/Data/ODBC/ODBCException.h"

#include <Poco/AutoPtr.h>
#include <Poco/DateTime.h>
#include <Poco/DateTimeFormatter.h>
#include <Poco/Exception.h>
#include <Poco/NumberFormatter.h>
#include <Poco/NumberParser.h>
#include <Poco/Util/IniFileConfiguration.h>

#include <Poco/AsyncChannel.h>
#include <Poco/AutoPtr.h>
#include <Poco/File.h>
#include <Poco/FormattingChannel.h>
#include <Poco/Logger.h>
#include <Poco/Path.h>
#include <Poco/PatternFormatter.h>
#include <Poco/SimpleFileChannel.h>
#include <Poco/StringTokenizer.h>

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/thread/thread.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/random/random_device.hpp>
#include <boost/random/uniform_int_distribution.hpp>
#include <boost/regex.hpp>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>

#include "uniqueid.h"

#include "protocols/abstract_protocol.h"
#include "protocols/db_basic.h"
#include "protocols/db_basic_v2.h"
#include "protocols/db_custom_v2.h"
#include "protocols/db_procedure.h"
#include "protocols/db_procedure_v2.h"
#include "protocols/db_raw.h"
#include "protocols/db_raw_v2.h"
#include "protocols/db_raw_no_extra_quotes.h"
#include "protocols/db_raw_no_extra_quotes_v2.h"
#include "protocols/log.h"
#include "protocols/misc.h"


void DBPool::customizeSession (Poco::Data::Session& session)
{
	try
	{
		session.setProperty("maxRetryAttempts", 100);
	}
	catch (Poco::Data::NotSupportedException&)
	{
	}
}


Ext::Ext(void) {
	mgr.reset (new IdManager);
	extDB_lock = false;

	Poco::DateTime now;
	Poco::Path log_path;
	log_path.pushDirectory("extDB");
	log_path.pushDirectory("logs");
	log_path.pushDirectory(Poco::DateTimeFormatter::format(now, "%Y"));
	log_path.pushDirectory(Poco::DateTimeFormatter::format(now, "%n"));
	log_path.pushDirectory(Poco::DateTimeFormatter::format(now, "%d"));
	Poco::File(log_path).createDirectories();
	log_path.setFileName(Poco::DateTimeFormatter::format(now, "%H-%M-%S.log"));
	
	pChannel = new Poco::SimpleFileChannel;
	pChannel->setProperty("path", log_path.toString());
	pChannel->setProperty("rotation", "10 M");

	pAsync = new Poco::AsyncChannel(pChannel);
	
	pPF = new Poco::PatternFormatter;
	pPF->setProperty("pattern", "%Y-%m-%d %H:%M:%S:%F %s: %p: %t");
	pPFC = new Poco::FormattingChannel(pPF, pAsync);
	
	Poco::Logger::root().setChannel(pPFC);
	pLogger = &Poco::Logger::get("extDB");

	bool conf_found = false;
	bool conf_randomized = false;
	
	Poco::File conf_file("extdb-conf.ini");
	
	if (conf_file.exists())
	{
		if (conf_file.isFile())
		{
			conf_found = true;
			pConf = (new Poco::Util::IniFileConfiguration("extdb-conf.ini"));
		}
	}
	else
    {
		std::vector<std::string> file_list;
		Poco::File(Poco::Path().current()).list(file_list);
		
		// Search for Randomize Config File -- Legacy Security Support For Arma2Servers
			// TODO: WINDOWS ONLY ifdef endif
        boost::regex expression("extdb-conf.*ini");
        for(std::vector<std::string>::iterator it = file_list.begin(); it != file_list.end(); ++it)
        {
			if (Poco::File(*it).isFile())
			{
				if(boost::regex_search(*it, expression))
				{
					conf_found = true;
					conf_randomized = true;
					pConf = new Poco::Util::IniFileConfiguration(*it);  // Load Randomized Conf
					break;
				}
			}
		}
    }

	pLogger->information("Version: " + version());
	
	if (!conf_found) 
	{
		#ifdef TESTING
			std::cout << "extDB: Unable to find extdb-conf.ini" << std::endl;
		#endif

		pLogger->information("Unable to find extdb-conf.ini");
		// Kill Server no config file found -- Evil
		// TODO: See if we can extension limp along with bad config ?
        std::exit(EXIT_FAILURE);
	}
	else
	{
		
		#ifdef TESTING
			std::cout << "extDB: Found extdb-conf.ini" << std::endl;
		#endif
		pLogger->information("Found extdb-conf.ini");

		steam_api_key = pConf->getString("Main.Steam_WEB_API_KEY", "");

		// Start Threads + ASIO
		max_threads = pConf->getInt("Main.Threads", 0);
        if (max_threads <= 0)
        {
            max_threads = boost::thread::hardware_concurrency();
        }
		io_work_ptr.reset(new boost::asio::io_service::work(io_service));
        for (int i = 0; i < max_threads; ++i)
        {
            threads.create_thread(boost::bind(&boost::asio::io_service::run, &io_service));
			#ifdef TESTING
				std::cout << "extDB: Creating Worker Thread +1" << std::endl ;
			#endif
			pLogger->information("Creating Worker Thread +1");
        }

		// Load Logging Filter Options
		#ifdef TESTING
			std::cout << "extDB: Loading Log Settings" << std::endl;
		#endif
		
		std::string level = pConf->getString("Logging.Level", "");
		
		if (boost::iequals(level, "none") == 1)
		{
			Poco::Logger::root().setLevel(0);
			pLogger->setLevel(0);
		}
		else if (boost::iequals(level, "fatal") == 1)
		{
			Poco::Logger::root().setLevel(Poco::Message::PRIO_FATAL);
			pLogger->setLevel(Poco::Message::PRIO_FATAL);
		}
		else if (boost::iequals(level, "critical") == 1)
		{
			Poco::Logger::root().setLevel(Poco::Message::PRIO_CRITICAL);
			pLogger->setLevel(Poco::Message::PRIO_CRITICAL);
		}
		else if (boost::iequals(level, "error") == 1)
		{
			Poco::Logger::root().setLevel(Poco::Message::PRIO_ERROR);
			pLogger->setLevel(Poco::Message::PRIO_ERROR);
		}
		else if (boost::iequals(level, "warning") == 1)
		{
			Poco::Logger::root().setLevel(Poco::Message::PRIO_WARNING);
			pLogger->setLevel(Poco::Message::PRIO_WARNING);
		}
		else if (boost::iequals(level, "notice") == 1)
		{
			Poco::Logger::root().setLevel(Poco::Message::PRIO_NOTICE);
			pLogger->setLevel(Poco::Message::PRIO_NOTICE);
		}
		else if (boost::iequals(level, "information") == 1)
		{
			Poco::Logger::root().setLevel(Poco::Message::PRIO_INFORMATION);
			pLogger->setLevel(Poco::Message::PRIO_INFORMATION);
		}
		else if (boost::iequals(level, "debug") == 1)
		{
			Poco::Logger::root().setLevel(Poco::Message::PRIO_DEBUG);
			pLogger->setLevel(Poco::Message::PRIO_DEBUG);
		}
		else if (boost::iequals(level, "trace") == 1)
		{
			Poco::Logger::root().setLevel(Poco::Message::PRIO_TRACE);
			pLogger->setLevel(Poco::Message::PRIO_TRACE);
		}
		else
		{
			// Default Value
			Poco::Logger::root().setLevel(Poco::Message::PRIO_INFORMATION);
			pLogger->setLevel(Poco::Message::PRIO_INFORMATION);
			pLogger->warning("No Config Option Logging - Level Found, Using Default Value -> Information");
		}


		#ifdef TESTING
//			std::cout << "extDB: Loading Rcon Settings" << std::endl;
//			rcon.init(pConf->getInt("Main.RconPort", 2302), pConf->getString("Main.RconPassword", "password"));
		#endif
		//pLogger->information("Loading Rcon Settings");
		
		
		if ((pConf->getBool("Main.Randomize Config File", false)) && (!conf_randomized))
		// Only Gonna Randomize Once, Keeps things Simple
		{
			std::string chars("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
							  "1234567890");
			// Skipping Lowercase, this function only for arma2 + extensions only available on windows.
			boost::random::random_device rng;
			boost::random::uniform_int_distribution<> index_dist(0, chars.size() - 1);
			
			std::string randomized_filename = "extdb-conf-";
			for(int i = 0; i < 8; ++i) {
				randomized_filename += chars[index_dist(rng)];
			}
			randomized_filename += ".ini";
			Poco::File("extdb-conf.ini").renameTo(randomized_filename);
		}
    }
}

Ext::~Ext(void)
{
    stop();
}

void Ext::stop()
{
	#ifdef TESTING
		std::cout << "extDB: Stopping Please Wait..." << std::endl;
	#endif
	pLogger->information("Stopping Please Wait...");

	io_service.stop();
    threads.join_all();
    unordered_map_protocol.clear();

    if (boost::iequals(db_conn_info.db_type, std::string("MySQL")) == 1)
        Poco::Data::MySQL::Connector::unregisterConnector();
    else if (boost::iequals(db_conn_info.db_type, std::string ("ODBC")) == 1)
        Poco::Data::ODBC::Connector::unregisterConnector();
    else if (boost::iequals(db_conn_info.db_type, "SQLite") == 1)
        Poco::Data::SQLite::Connector::unregisterConnector();

	pLogger->information("Stopped");
}

void Ext::connectDatabase(char *output, const int &output_size, const std::string &conf_option)
{
	// TODO ADD Code to check for database already initialized !!!!!!!!!!!
    try
    {
        if (pConf->hasOption(conf_option + ".Type"))
        {
            // Database
            db_conn_info.db_type = pConf->getString(conf_option + ".Type");
            std::string db_name = pConf->getString(conf_option + ".Name");

            db_conn_info.min_sessions = pConf->getInt(conf_option + ".minSessions", 1);
            if (db_conn_info.min_sessions <= 0)
            {
                db_conn_info.min_sessions = 1;
            }
            db_conn_info.min_sessions = pConf->getInt(conf_option + ".maxSessions", 1);
            if (db_conn_info.max_sessions <= 0)
            {
                db_conn_info.max_sessions = max_threads;
            }
			
            db_conn_info.idle_time = pConf->getInt(conf_option + ".idleTime");

			#ifdef TESTING
				std::cout << "extDB: Database Type: " << db_conn_info.db_type << std::endl;
			#endif
			pLogger->information("Database Type: " + db_conn_info.db_type);

            if ( (boost::iequals(db_conn_info.db_type, std::string("MySQL")) == 1) || (boost::iequals(db_conn_info.db_type, std::string("ODBC")) == 1) )
            {
                std::string username = pConf->getString(conf_option + ".Username");
                std::string password = pConf->getString(conf_option + ".Password");

                std::string ip = pConf->getString(conf_option + ".IP");
                std::string port = pConf->getString(conf_option + ".Port");
				
				db_conn_info.connection_str = "host=" + ip + ";port=" + port + ";user=" + username + ";password=" + password + ";db=" + db_name + ";auto-reconnect=true";
				
                if (boost::iequals(db_conn_info.db_type, std::string("MySQL")) == 1)
                {
					db_conn_info.db_type = "MySQL";
                    Poco::Data::MySQL::Connector::registerConnector();
					std::string compress = pConf->getString(conf_option + ".Compress", "false");
					if (boost::iequals(compress, "true") == 1)
					{
						db_conn_info.connection_str = db_conn_info.connection_str + "compress=true";
					}
                }
				else
                {
					db_conn_info.db_type = "ODBC";
                    Poco::Data::ODBC::Connector::registerConnector();
				}

                db_pool.reset(new DBPool(db_conn_info.db_type, 
															db_conn_info.connection_str, 
															db_conn_info.min_sessions, 
															db_conn_info.max_sessions, 
															db_conn_info.idle_time));
                if (db_pool->get().isConnected())
                {
					#ifdef TESTING
						std::cout << "extDB: Database Session Pool Started" << std::endl;
					#endif
					pLogger->information("Database Session Pool Started");
                    std::strcpy(output, "[1]");
                }
                else
                {
					#ifdef TESTING
						std::cout << "extDB: Database Session Pool Failed" << std::endl;
					#endif
					pLogger->critical("Database Session Pool Failed");
					std::strcpy(output, "[0,\"Database Session Pool Failed\"]");
                }
            }
            else if (boost::iequals(db_conn_info.db_type, "SQLite") == 1)
            {
				db_conn_info.db_type = "SQLite";
                Poco::Data::SQLite::Connector::registerConnector();
				Poco::Path db_path;
				db_path.pushDirectory("extDB");
				db_path.pushDirectory("sqlite");
				db_path.setFileName(db_name);
                db_conn_info.connection_str = db_path.toString();

                db_pool.reset(new DBPool(db_conn_info.db_type, 
															db_conn_info.connection_str, 
															db_conn_info.min_sessions, 
															db_conn_info.max_sessions, 
															db_conn_info.idle_time));

                if (db_pool->get().isConnected())
                {
					#ifdef TESTING
						std::cout << "extDB: Database Session Pool Started" << std::endl;
					#endif
					pLogger->information("Database Session Pool Started");
                    std::strcpy(output, "[1]");
                }
                else
                {
					#ifdef TESTING
						std::cout << "extDB: Database Session Pool Failed" << std::endl;
					#endif
					pLogger->critical("Database Session Pool Failed");
                    std::strcpy(output, "[0,\"Database Session Pool Failed\"]");
                }
            }
            else
            {
				#ifdef TESTING
					std::cout << "extDB: No Database Engine Found for " << db_name << "." << std::endl;
				#endif 
				pLogger->error("No Database Engine Found for " + db_name + ".");
				std::strcpy(output, "[0,\"Unknown Database Type\"]");
            }
        }
        else
        {
			#ifdef TESTING
				std::cout << "extDB: WARNING No Config Option Found: " << conf_option << "." << std::endl;
			#endif
			pLogger->error("No Config Option Found: " + conf_option + ".");
			std::strcpy(output, "[0,\"No Config Option Found\"]");
        }
    }
    catch (Poco::Exception& e)
    {
		#ifdef TESTING
			std::cout << "extDB: Database Setup Failed: " << e.displayText() << std::endl;
		#endif
		pLogger->error("Database Setup Failed: " + e.displayText());
        std::exit(EXIT_FAILURE);
    }
}


std::string Ext::version() const
{
    return "16";
}


std::string Ext::getAPIKey()
{
	return steam_api_key;
}


int Ext::getUniqueID_mutexlock()
{
    boost::lock_guard<boost::mutex> lock(mutex_unique_id);
    return mgr.get()->AllocateId();
}


void Ext::freeUniqueID_mutexlock(const int &unique_id)
{
    boost::lock_guard<boost::mutex> lock(mutex_unique_id);
    mgr.get()->FreeId(unique_id);
}

Poco::Data::Session Ext::getDBSession_mutexlock()
// Gets available DB Session (mutex lock)
{
	try
	{
		boost::lock_guard<boost::mutex> lock(mutex_db_pool);
		Poco::Data::Session free_session =  db_pool->get();
		return free_session;
	}
	catch (Poco::Data::SessionPoolExhaustedException&)
	//	Exceptiontal Handling in event of scenario if all asio worker threads are busy using all db connections
	//		And there is SYNC call using db & db_pool = exhausted
	{
		Poco::Data::Session new_session(db_conn_info.db_type, db_conn_info.connection_str);
		return new_session;
	}
}

std::string Ext::getDBType()
{
	return db_conn_info.db_type;
}

void Ext::getResult_mutexlock(const int &unique_id, char *output, const int &output_size)
// Gets Result String from unordered map array
//   If length of String = 0, sends arma "", and removes entry from unordered map array
//   If <=, then sends output to arma
//   If >, then sends 1 part to arma + stores rest.
{
	boost::lock_guard<boost::mutex> lock(mutex_unordered_map_results); // TODO Try to make Mutex Lock smaller
	boost::unordered_map<int, std::string>::const_iterator it = unordered_map_results.find(unique_id);
	if (it == unordered_map_results.end()) // NO UNIQUE ID or WAIT
	{
		if (unordered_map_wait.count(unique_id) == 0)
		{
			std::strcpy(output, (""));
		}
		else
		{
			std::strcpy(output, ("[3]"));
		}
	}
	else if (it->second.empty()) // END of MSG
	{
		unordered_map_results.erase(unique_id);
		freeUniqueID_mutexlock(unique_id);
		std::strcpy(output, (""));
	}
	else // SEND MSG (Part)
	{
		std::string msg = it->second.substr(0, output_size-9);
		std::strcpy(output, msg.c_str());
		if (it->second.length() > (output_size-9))
		{
			unordered_map_results[unique_id] = it->second.substr(output_size-9);
		}
		else
		{
			unordered_map_results[unique_id].clear();
		}
	}
}


void Ext::saveResult_mutexlock(const std::string &result, const int &unique_id)
// Stores Result String  in a unordered map array.
//   Used when string > arma output char
{
	boost::lock_guard<boost::mutex> lock(mutex_unordered_map_results);
	unordered_map_results[unique_id] = "[1," + result + "]";
	unordered_map_wait.erase(unique_id);
}


void Ext::addProtocol(char *output, const int &output_size, const std::string &protocol, const std::string &protocol_name, const std::string &init_data)
{
	{
		// TODO Implement Poco ClassLoader -- dayz hive ext has it to load database dll
		boost::lock_guard<boost::mutex> lock(mutex_unordered_map_protocol);
		if (boost::iequals(protocol, std::string("MISC")) == 1)
		{
			unordered_map_protocol[protocol_name] = boost::shared_ptr<AbstractProtocol> (new MISC());
			if (!unordered_map_protocol[protocol_name].get()->init(this, init_data))
			// Remove Class Instance if Failed to Load
			{
				unordered_map_protocol.erase(protocol_name);
				std::strcpy(output, "[0,\"Failed to Load Protocol\"]");
			}
			else
			{
				std::strcpy(output, "[1]");
			}
		}
		else if (boost::iequals(protocol, std::string("DB_BASIC")) == 1)
		{
			unordered_map_protocol[protocol_name] = boost::shared_ptr<AbstractProtocol> (new DB_BASIC());
			if (!unordered_map_protocol[protocol_name].get()->init(this, init_data))
			// Remove Class Instance if Failed to Load
			{
				unordered_map_protocol.erase(protocol_name);
				std::strcpy(output, "[0,\"Failed to Load Protocol\"]");
			}
			else
			{
				std::strcpy(output, "[1]");
				pLogger->warning("DB_BASIC is Deprecated... Update SQF code for DB_BASIC_V2");
			}
		}
		else if (boost::iequals(protocol, std::string("DB_BASIC_V2")) == 1)
		{
			unordered_map_protocol[protocol_name] = boost::shared_ptr<AbstractProtocol> (new DB_BASIC_V2());
			if (!unordered_map_protocol[protocol_name].get()->init(this, init_data))
			// Remove Class Instance if Failed to Load
			{
				unordered_map_protocol.erase(protocol_name);
				std::strcpy(output, "[0,\"Failed to Load Protocol\"]");
			}
			else
			{
				std::strcpy(output, "[1]");
			}
		}
		else if (boost::iequals(protocol, std::string("DB_PROCEDURE")) == 1)
		{
			unordered_map_protocol[protocol_name] = boost::shared_ptr<AbstractProtocol> (new DB_PROCEDURE());
			if (!unordered_map_protocol[protocol_name].get()->init(this, init_data))
			// Remove Class Instance if Failed to Load
			{
				unordered_map_protocol.erase(protocol_name);
				std::strcpy(output, "[0,\"Failed to Load Protocol\"]");
			}
			else
			{
				std::strcpy(output, "[1]");
				pLogger->warning("DB_PROCEDURE is Deprecated... Update SQF code for DB_PROCEDURE_V2");
			}
		}
		else if (boost::iequals(protocol, std::string("DB_PROCEDURE_V2")) == 1)
		{
			unordered_map_protocol[protocol_name] = boost::shared_ptr<AbstractProtocol> (new DB_PROCEDURE_V2());
			if (!unordered_map_protocol[protocol_name].get()->init(this, init_data))
			// Remove Class Instance if Failed to Load
			{
				unordered_map_protocol.erase(protocol_name);
				std::strcpy(output, "[0,\"Failed to Load Protocol\"]");
			}
			else
			{
				std::strcpy(output, "[1]");
			}
		}
		else if (boost::iequals(protocol, std::string("DB_RAW")) == 1)
		{
			unordered_map_protocol[protocol_name] = boost::shared_ptr<AbstractProtocol> (new DB_RAW());
			if (!unordered_map_protocol[protocol_name].get()->init(this, init_data))
			// Remove Class Instance if Failed to Load
			{
				unordered_map_protocol.erase(protocol_name);
				std::strcpy(output, "[0,\"Failed to Load Protocol\"]");
			}
			else
			{
				std::strcpy(output, "[1]");
				pLogger->warning("DB_RAW is Deprecated... Update SQF code for DB_RAW_V2");
			}
		}
		else if (boost::iequals(protocol, std::string("DB_RAW_V2")) == 1)
		{
			unordered_map_protocol[protocol_name] = boost::shared_ptr<AbstractProtocol> (new DB_RAW_V2());
			if (!unordered_map_protocol[protocol_name].get()->init(this, init_data))
			// Remove Class Instance if Failed to Load
			{
				unordered_map_protocol.erase(protocol_name);
				std::strcpy(output, "[0,\"Failed to Load Protocol\"]");
			}
			else
			{
				std::strcpy(output, "[1]");
			}
		}
		else if (boost::iequals(protocol, std::string("DB_RAW_NO_EXTRA_QUOTES")) == 1)
		{
			unordered_map_protocol[protocol_name] = boost::shared_ptr<AbstractProtocol> (new DB_RAW_NO_EXTRA_QUOTES());
			if (!unordered_map_protocol[protocol_name].get()->init(this, init_data))
			// Remove Class Instance if Failed to Load
			{
				unordered_map_protocol.erase(protocol_name);
				std::strcpy(output, "[0,\"Failed to Load Protocol\"]");
			}
			else
			{
				std::strcpy(output, "[1]");
				pLogger->warning("DB_RAW_NO_EXTRA_QUOTES is Deprecated... Update SQF code for DB_RAW_NO_EXTRA_QUOTES_V2");
			}
		}
		else if (boost::iequals(protocol, std::string("DB_RAW_NO_EXTRA_QUOTES_V2")) == 1)
		{
			unordered_map_protocol[protocol_name] = boost::shared_ptr<AbstractProtocol> (new DB_RAW_NO_EXTRA_QUOTES_V2());
			if (!unordered_map_protocol[protocol_name].get()->init(this, init_data))
			// Remove Class Instance if Failed to Load
			{
				unordered_map_protocol.erase(protocol_name);
				std::strcpy(output, "[0,\"Failed to Load Protocol\"]");
			}
			else
			{
				std::strcpy(output, "[1]");
			}
		}
		else if (boost::iequals(protocol, std::string("DB_CUSTOM_V2")) == 1)
		{
			unordered_map_protocol[protocol_name] = boost::shared_ptr<AbstractProtocol> (new DB_CUSTOM_V2());
			if (!unordered_map_protocol[protocol_name].get()->init(this, init_data))
			// Remove Class Instance if Failed to Load
			{
				unordered_map_protocol.erase(protocol_name);
				std::strcpy(output, "[0,\"Failed to Load Protocol\"]");
			}
			else
			{
				std::strcpy(output, "[1]");
			}
		}
		else if (boost::iequals(protocol, std::string("LOG")) == 1)
		{
			unordered_map_protocol[protocol_name] = boost::shared_ptr<AbstractProtocol> (new LOG());
			if (!unordered_map_protocol[protocol_name].get()->init(this, init_data))
			// Remove Class Instance if Failed to Load
			{
				unordered_map_protocol.erase(protocol_name);
				std::strcpy(output, "[0,\"Failed to Load Protocol\"]");
			}
			else
			{
				std::strcpy(output, "[1]");
			}
		}
		else
		{
			std::strcpy(output, "[0,\"Error Unknown Protocol\"]");
		}
	}
}


void Ext::syncCallProtocol(char *output, const int &output_size, const std::string &protocol, const std::string &data)
// Sync callPlugin
{
	boost::unordered_map< std::string, boost::shared_ptr<AbstractProtocol> >::const_iterator itr = unordered_map_protocol.find(protocol);
    if (itr == unordered_map_protocol.end())
    {
        std::strcpy(output, ("[0,\"Error Unknown Protocol\"]"));
    }
    else
    {
		// Checks if Result String will fit into arma output char
		//   If <=, then sends output to arma
		//   if >, then sends ID Message arma + stores rest. (mutex locks)
		std::string result;
		result.reserve(2000);
        itr->second->callProtocol(this, data, result);
		if (result.length() <= (output_size-9))
		{
			std::strcpy(output, ("[1, " + result + "]").c_str());
		}
		else
		{
			const int unique_id = getUniqueID_mutexlock();
			saveResult_mutexlock(result, unique_id);
			std::strcpy(output, ("[2,\"" + Poco::NumberFormatter::format(unique_id) + "\"]").c_str());
		}
    }
}


void Ext::onewayCallProtocol(const std::string protocol, const std::string data)
// ASync callProtocol
{
	boost::unordered_map< std::string, boost::shared_ptr<AbstractProtocol> >::const_iterator itr = unordered_map_protocol.find(protocol);
    if (itr != unordered_map_protocol.end())
    {
		std::string result;
		result.reserve(2000);
        itr->second->callProtocol(this, data, result);
    }
}


void Ext::asyncCallProtocol(const std::string protocol, const std::string data, const int unique_id)
// ASync + Save callProtocol
// We check if Protocol exists here, since its a thread (less time spent blocking arma) and it shouldnt happen anyways
{
	std::string result;
	result.reserve(2000);
	unordered_map_protocol[protocol].get()->callProtocol(this, data, result);
	saveResult_mutexlock(result, unique_id);
}


void Ext::callExtenion(char *output, const int &output_size, const char *function)
{
    try
    {
		#ifdef DEBUG_LOGGING
			pLogger->trace("Extension Input from Server: " +  std::string(function));
		#endif
		const std::string input_str(function);
		if (input_str.length() <= 2)
		{
			std::strcpy(output, ("[0,\"Error Invalid Message, (Message to short)\"]"));
		}
		else
		{
			const std::string sep_char(":");

			// Async / Sync
			const int async = Poco::NumberParser::parse(input_str.substr(0,1));

			switch (async)  // TODO Profile using Numberparser versus comparsion of char[0] + if statement checking length of *function
			{
				case 2: //ASYNC + SAVE
				{
					// Protocol
					const std::string::size_type found = input_str.find(sep_char,2);

					if (found==std::string::npos)  // Check Invalid Format
					{
						std::strcpy(output, ("[0,\"Error Invalid Format\"]"));
					}
					else
					{
						bool found_procotol = false;
						const std::string protocol = input_str.substr(2,(found-2));
						// Data
						std::string data = input_str.substr(found+1);
						int unique_id = getUniqueID_mutexlock();
						{
							boost::lock_guard<boost::mutex> lock(mutex_unordered_map_results);
							// Check for Protocol Name Exists
							if (unordered_map_protocol.find(protocol) == unordered_map_protocol.end())
							{
								std::strcpy(output, ("[0,\"Error Unknown Protocol\"]"));
							}
							else
							{
								unordered_map_wait[unique_id] = true;
								found_procotol = true;
							}
						}
						// Only Add Job to Work Queue + Return ID if Protocol Name exists.
						if (found_procotol)
						{
							io_service.post(boost::bind(&Ext::asyncCallProtocol, this, protocol, data, unique_id));
							std::strcpy(output, (("[2,\"" + Poco::NumberFormatter::format(unique_id) + "\"]")).c_str());
						}
					}
					break;
				}
				case 5: // GET
				{
					const int unique_id = Poco::NumberParser::parse(input_str.substr(2));
					getResult_mutexlock(unique_id, output, output_size);
					break;
				}
				case 1: //ASYNC
				{
					// Protocol
					const std::string::size_type found = input_str.find(sep_char,2);

					if (found==std::string::npos)  // Check Invalid Format
					{
						std::strcpy(output, ("[0,\"Error Invalid Format\"]"));
					}
					else
					{
						const std::string protocol = input_str.substr(2,(found-2));
						// Data
						const std::string data = input_str.substr(found+1);
						io_service.post(boost::bind(&Ext::onewayCallProtocol, this, protocol, data));
						std::strcpy(output, "[1]");
					}
					break;
				}
				case 0: //SYNC
				{
					// Protocol
					const std::string::size_type found = input_str.find(sep_char,2);

					if (found==std::string::npos)  // Check Invalid Format
					{
						std::strcpy(output, ("[0,\"Error Invalid Format\"]"));
					}
					else
					{
						const std::string protocol = input_str.substr(2,(found-2));
						// Data
						const std::string data = input_str.substr(found+1);
						syncCallProtocol(output, output_size, protocol, data);
					}
					break;
				}
				case 9:
				{
					if (!extDB_lock)
					{
						// Protocol

						Poco::StringTokenizer tokens(input_str, ":");
						std::size_t token_count = tokens.count(); // TODO CHECK !!!!!!!!
						
						switch (token_count)
						{
							case 2:
								// LOCK / VERSION
								if (tokens[1] == "VERSION")
								{
									std::strcpy(output, version().c_str());
								}
								else if (tokens[1] == "LOCK")
								{
									extDB_lock = true;
								}
								break;
							case 3:
								// DATABASE
								connectDatabase(output, output_size, tokens[2]);
								break;
							case 4:
								// ADD PROTOCOL
								addProtocol(output, output_size, tokens[2], tokens[3], "");
								break;
							case 5:
								//ADD PROTOCOL
								addProtocol(output, output_size, tokens[2], tokens[3], tokens[4]);
								break;
							default:
								// Invalid Format
								std::strcpy(output, ("[0,\"Error Invalid Format\"]"));
						}
					}
					break;
				}
				default:
				{
					std::strcpy(output, ("[0,\"Error Invalid Message\"]"));
				}
			}
        }
    }
    catch (Poco::Exception& e)
    {
        std::strcpy(output, ("[0,\"Error Invalid Message\"]"));
		#ifdef TESTING
			std::cout << "extDB: Error: " << e.displayText() << std::endl;
		#endif
		pLogger->error("extDB: Error: " + e.displayText());
    }
}


#ifdef TEST_APP
int main(int nNumberofArgs, char* pszArgs[])
{
	std::cout << std::endl << "Welcome to extDB Test Application : " << std::endl;
	std::cout << "    This application has 4096 char limited input." << std::endl;
	std::cout << "         Extension doesn't have this problem" << std::endl;
	std::cout << " To exit type 'quit'" << std::endl << std::endl;
    Ext *extension;
    extension = (new Ext());
    char result[4096];
    for (;;) {
        char input_str[4096];
		std::cin.getline(input_str, sizeof(input_str));
        if (std::string(input_str) == "quit")
        {
            break;
        }
        else
        {
            extension->callExtenion(result, 80, input_str);
            std::cout << "extDB: " << result << std::endl;
        }
    }
	std::cout << "extDB Test: Quitting Please Wait" << std::endl;
	extension->stop();
	//delete extension;
    return 0;
}
#endif