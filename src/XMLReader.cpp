#include "XMLReader.h"
#include "SystemData.h"
#include <boost/filesystem.hpp>
#include "Log.h"
#include "Settings.h"

#define USE_SQLITE_GAMELIST

namespace fs = boost::filesystem;

// example: removeCommonPath("/home/pi/roms/nes/foo/bar.nes", "/home/pi/roms/nes/") returns "foo/bar.nes"
fs::path removeCommonPath(const fs::path& path, const fs::path& relativeTo, bool& contains)
{
	fs::path p = fs::canonical(path);
	fs::path r = fs::canonical(relativeTo);

	if(p.root_path() != r.root_path())
	{
		contains = false;
		return p;
	}

	fs::path result;

	// find point of divergence
	auto itr_path = p.begin();
    auto itr_relative_to = r.begin();
    while(*itr_path == *itr_relative_to && itr_path != p.end() && itr_relative_to != r.end())
	{
        ++itr_path;
        ++itr_relative_to;
    }

	if(itr_relative_to != r.end())
	{
		contains = false;
		return p;
	}

	while(itr_path != p.end())
	{
		if(*itr_path != fs::path("."))
			result = result / *itr_path;

		++itr_path;
	}

	contains = true;
	return result;
}

FileData* findOrCreateFile(SystemData* system, const boost::filesystem::path& path, FileType type)
{
	// first, verify that path is within the system's root folder
	FileData* root = system->getRootFolder();
	
	bool contains = false;
	fs::path relative = removeCommonPath(path, root->getPath(), contains);
	if(!contains)
	{
		LOG(LogError) << "File path \"" << path << "\" is outside system path \"" << system->getStartPath() << "\"";
		return NULL;
	}

	auto path_it = relative.begin();
	FileData* treeNode = root;
	bool found = false;
	while(path_it != relative.end())
	{
		const std::vector<FileData*>& children = treeNode->getChildren();
		found = false;
		for(auto child_it = children.begin(); child_it != children.end(); child_it++)
		{
			if((*child_it)->getPath().filename() == *path_it)
			{
				treeNode = *child_it;
				found = true;
				break;
			}
		}

		// this is the end
		if(path_it == --relative.end())
		{
			if(found)
				return treeNode;

			if(type == FOLDER)
			{
				LOG(LogWarning) << "gameList: folder doesn't already exist, won't create";
				return NULL;
			}

			FileData* file = new FileData(type, path, system);
			treeNode->addChild(file);
			return file;
		}

		if(!found)
		{
			// don't create folders unless it's leading up to a game
			// if type is a folder it's gonna be empty, so don't bother
			if(type == FOLDER)
			{
				LOG(LogWarning) << "gameList: folder doesn't already exist, won't create";
				return NULL;
			}
			
			// create missing folder
			FileData* folder = new FileData(FOLDER, treeNode->getPath().stem() / *path_it, system);
			treeNode->addChild(folder);
			treeNode = folder;
		}

		path_it++;
	}

	return NULL;
}

#ifdef USE_XML_GAMELIST

#include "pugiXML/pugixml.hpp"

void parseGamelist(SystemData* system)
{
	std::string xmlpath = system->getGamelistPath(false);

	if(!boost::filesystem::exists(xmlpath))
		return;

	LOG(LogInfo) << "Parsing XML file \"" << xmlpath << "\"...";

	pugi::xml_document doc;
	pugi::xml_parse_result result = doc.load_file(xmlpath.c_str());

	if(!result)
	{
		LOG(LogError) << "Error parsing XML file \"" << xmlpath << "\"!\n	" << result.description();
		return;
	}

	pugi::xml_node root = doc.child("gameList");
	if(!root)
	{
		LOG(LogError) << "Could not find <gameList> node in gamelist \"" << xmlpath << "\"!";
		return;
	}

	const char* tagList[2] = { "game", "folder" };
	FileType typeList[2] = { GAME, FOLDER };
	for(int i = 0; i < 2; i++)
	{
		const char* tag = tagList[i];
		FileType type = typeList[i];
		for(pugi::xml_node fileNode = root.child(tag); fileNode; fileNode = fileNode.next_sibling(tag))
		{
			boost::filesystem::path path = boost::filesystem::path(fileNode.child("path").text().get());
			
			if(!boost::filesystem::exists(path))
			{
				LOG(LogWarning) << "File \"" << path << "\" does not exist! Ignoring.";
				continue;
			}

			FileData* file = findOrCreateFile(system, path, type);
			if(!file)
			{
				LOG(LogError) << "Error finding/creating FileData for \"" << path << "\", skipping.";
				continue;
			}

			//load the metadata
			std::string defaultName = file->metadata.get("name");
			file->metadata = MetaDataList::createFromXML(GAME_METADATA, fileNode);

			//make sure name gets set if one didn't exist
			if(file->metadata.get("name").empty())
				file->metadata.set("name", defaultName);
		}
	}
}

void addGameDataNode(pugi::xml_node& parent, const FileData* game, SystemData* system)
{
	//create game and add to parent node
	pugi::xml_node newGame = parent.append_child("game");

	//write metadata
	game->metadata.appendToXML(newGame, true);
	
	if(newGame.children().begin() == newGame.child("name") //first element is name
		&& ++newGame.children().begin() == newGame.children().end() //theres only one element
		&& newGame.child("name").text().get() == getCleanFileName(game->getPath())) //the name is the default
	{
		//if the only info is the default name, don't bother with this node
		parent.remove_child(newGame);
	}else{
		//there's something useful in there so we'll keep the node, add the path
		newGame.prepend_child("path").text().set(game->getPath().generic_string().c_str());
	}
}

void addFileDataNode(pugi::xml_node& parent, const FileData* file, const char* tag, SystemData* system)
{
	//create game and add to parent node
	pugi::xml_node newNode = parent.append_child(tag);

	//write metadata
	file->metadata.appendToXML(newNode, true);
	
	if(newNode.children().begin() == newNode.child("name") //first element is name
		&& ++newNode.children().begin() == newNode.children().end() //theres only one element
		&& newNode.child("name").text().get() == getCleanFileName(file->getPath())) //the name is the default
	{
		//if the only info is the default name, don't bother with this node
		//delete it and ultimately do nothing
		parent.remove_child(newNode);
	}else{
		//there's something useful in there so we'll keep the node, add the path
		newNode.prepend_child("path").text().set(file->getPath().generic_string().c_str());
	}
}

void updateGamelist(SystemData* system)
{
	//We do this by reading the XML again, adding changes and then writing it back,
	//because there might be information missing in our systemdata which would then miss in the new XML.
	//We have the complete information for every game though, so we can simply remove a game
	//we already have in the system from the XML, and then add it back from its GameData information...

	if(Settings::getInstance()->getBool("IgnoreGamelist"))
		return;

	pugi::xml_document doc;
	pugi::xml_node root;
	std::string xmlReadPath = system->getGamelistPath(false);

	if(boost::filesystem::exists(xmlReadPath))
	{
		//parse an existing file first
		pugi::xml_parse_result result = doc.load_file(xmlReadPath.c_str());
		
		if(!result)
		{
			LOG(LogError) << "Error parsing XML file \"" << xmlReadPath << "\"!\n	" << result.description();
			return;
		}

		root = doc.child("gameList");
		if(!root)
		{
			LOG(LogError) << "Could not find <gameList> node in gamelist \"" << xmlReadPath << "\"!";
			return;
		}
	}else{
		//set up an empty gamelist to append to
		root = doc.append_child("gameList");
	}


	//now we have all the information from the XML. now iterate through all our games and add information from there
	FileData* rootFolder = system->getRootFolder();
	if (rootFolder != nullptr)
	{
		//get only files, no folders
		std::vector<FileData*> files = rootFolder->getFilesRecursive(GAME | FOLDER);
		//iterate through all files, checking if they're already in the XML
		std::vector<FileData*>::const_iterator fit = files.cbegin();
		while(fit != files.cend())
		{
			const char* tag = ((*fit)->getType() == GAME) ? "game" : "folder";

			// check if the file already exists in the XML
			// if it does, remove it before adding
			for(pugi::xml_node fileNode = root.child(tag); fileNode; fileNode = fileNode.next_sibling(tag))
			{
				pugi::xml_node pathNode = fileNode.child("path");
				if(!pathNode)
				{
					LOG(LogError) << "<" << tag << "> node contains no <path> child!";
					continue;
				}

				fs::path nodePath(pathNode.text().get());
				fs::path gamePath((*fit)->getPath());
				if(nodePath == gamePath || (fs::exists(nodePath) && fs::exists(gamePath) && fs::equivalent(nodePath, gamePath)))
				{
					// found it
					root.remove_child(fileNode);
					break;
				}
			}

			// it was either removed or never existed to begin with; either way, we can add it now
			addFileDataNode(root, *fit, tag, system);

			++fit;
		}

		//now write the file

		//make sure the folders leading up to this path exist (or the write will fail)
		boost::filesystem::path xmlWritePath(system->getGamelistPath(true));
		boost::filesystem::create_directories(xmlWritePath.parent_path());

		if (!doc.save_file(xmlWritePath.c_str())) {
			LOG(LogError) << "Error saving gamelist.xml to \"" << xmlWritePath << "\" (for system " << system->getName() << ")!";
		}
	}else{
		LOG(LogError) << "Found no root folder for system \"" << system->getName() << "\"!";
	}
}

#endif // USE_XML_GAMELIST



// so I literally don't know anything about databases
// but this seems to work, although writes are really really slow
#ifdef USE_SQLITE_GAMELIST

#include "sqlite/sqlite3.h"

#define PATH_COLUMN 0

void parseGamelist(SystemData* system)
{
	std::string sqlpath = system->getGamelistPath(false, ".db");
	
	if(!boost::filesystem::exists(sqlpath))
		return;

	LOG(LogInfo) << "Parsing SQLite file \"" << sqlpath << "\"...";

	sqlite3* dbHandle = NULL;
	if(sqlite3_open(sqlpath.c_str(), &dbHandle) || dbHandle == NULL)
	{
		LOG(LogError) << "Error parsing SQLite file \"" << sqlpath << "\"!\n	" << sqlite3_errmsg(dbHandle);
		return;
	}


	struct Query
	{
		const std::vector<MetaDataDecl>& decl;
		FileType type;
		const char* queryString;
	};

	const unsigned int QUERY_COUNT = 2;
	Query queries[QUERY_COUNT] = {
		{getMDDByType(GAME_METADATA), GAME, "SELECT * FROM games"},
		{getMDDByType(FOLDER_METADATA), FOLDER, "SELECT * FROM folders"}
	};

	for(unsigned int query = 0; query < QUERY_COUNT; query++)
	{
		sqlite3_stmt* statement;
		if(sqlite3_prepare(dbHandle, queries[query].queryString, -1, &statement, NULL) != SQLITE_OK || statement == NULL)
		{
			LOG(LogError) << "Error preparing statement: " << sqlite3_errmsg(dbHandle);
			sqlite3_close(dbHandle);
			return;
		}

		while(true)
		{
			int status = sqlite3_step(statement);

			if(status == SQLITE_DONE)
				break;

			if(status == SQLITE_ROW)
			{
				// success
				// read path
				std::string path;
				path.reserve(sqlite3_column_bytes(statement, PATH_COLUMN) + 1);
				path = (const char*)sqlite3_column_text(statement, PATH_COLUMN);
				
				if(!boost::filesystem::exists(path))
				{
					LOG(LogWarning) << "File \"" << path << "\" does not exist! Ignoring.";
					continue;
				}

				FileData* file = findOrCreateFile(system, path, queries[query].type);
				if(!file)
				{
					LOG(LogError) << "Error finding/creating FileData for \"" << path << "\", skipping.";
					continue;
				}

				//load the metadata
				std::string defaultName = file->metadata.get("name");
				
				// load the metadata
				for(unsigned int i = 0; i < queries[query].decl.size(); i++)
				{
					int col = i+1;
					size_t numBytes = sqlite3_column_bytes(statement, col);
					std::string value;
					value.reserve(numBytes + 1);
					value = (const char*)sqlite3_column_text(statement, col);
					file->metadata.set(queries[query].decl.at(i).key, value);
				}

				//make sure name gets set if one didn't exist
				if(file->metadata.get("name").empty())
					file->metadata.set("name", defaultName);

				continue;
			}else{
				LOG(LogError) << "Error stepping through database: " << sqlite3_errmsg(dbHandle) << "\n(return code: " << status << ")";
				break;
			}

		}

		sqlite3_finalize(statement);
	}

	sqlite3_close(dbHandle);
}

void updateGamelist(SystemData* system)
{
	if(Settings::getInstance()->getBool("IgnoreGamelist"))
		return;

	std::string sqlReadPath = system->getGamelistPath(false, ".db");
	std::string sqlWritePath = system->getGamelistPath(true, ".db");

	//make sure the folders leading up to this path exist (or the write will fail)
	boost::filesystem::create_directories(fs::path(sqlWritePath).parent_path());

	if(boost::filesystem::exists(sqlReadPath) && sqlReadPath != sqlWritePath)
	{
		// copy an existing file first
		fs::copy(sqlReadPath, sqlWritePath);
	}

	// update the DB
	sqlite3* dbHandle = NULL;
	if(sqlite3_open(sqlWritePath.c_str(), &dbHandle) || dbHandle == NULL)
	{
		LOG(LogError) << "Error parsing SQLite file for update \"" << sqlWritePath << "\"!\n	" << sqlite3_errmsg(dbHandle);
		return;
	}

	struct Query
	{
		const std::vector<MetaDataDecl>& decl;
		const char* table;
	};
	const unsigned int QUERY_COUNT = 2;
	Query queries[QUERY_COUNT] = {
		{getMDDByType(GAME_METADATA), "games"},
		{getMDDByType(FOLDER_METADATA), "folders"}
	};

	// make sure the tables exist
	for(int i = 0; i < QUERY_COUNT; i++)
	{
		std::string statementStr = "CREATE TABLE IF NOT EXISTS ";
		statementStr += queries[i].table;
		statementStr += " ( path	TEXT	PRIMARY KEY	NOT NULL, ";
		for(unsigned int j = 0; j < queries[i].decl.size(); j++)
		{
			statementStr += queries[i].decl.at(j).key;
			statementStr += "	TEXT	NOT NULL";
			if(j != queries[i].decl.size() - 1)
				statementStr += ", ";
		}
		statementStr += ")";

		sqlite3_stmt* statement;
		if(sqlite3_prepare(dbHandle, statementStr.c_str(), -1, &statement, NULL) != SQLITE_OK || statement == NULL)
		{
			LOG(LogError) << "Error creating SQLite table " << queries[i].table << "!\nQuery: \"" << statementStr << "\"\n" << sqlite3_errmsg(dbHandle);
			sqlite3_close(dbHandle);
			return;
		}
		sqlite3_step(statement);
		sqlite3_finalize(statement);
	}


	FileData* rootFolder = system->getRootFolder();
	if (rootFolder != nullptr)
	{
		sqlite3_exec(dbHandle, "BEGIN TRANSACTION", NULL, NULL, NULL);

		//get only files, no folders
		std::vector<FileData*> files = rootFolder->getFilesRecursive(GAME | FOLDER);
		std::vector<FileData*>::const_iterator fit = files.cbegin();
		sqlite3_stmt* statement;
		while(fit != files.cend())
		{
			const char* table = ((*fit)->getType() == GAME) ? "games" : "folders";

			std::string stmtStr = "INSERT OR REPLACE INTO ";
			stmtStr += table;
			stmtStr += " VALUES(";

			const char* escapedPath = sqlite3_mprintf("%Q,", (*fit)->getPath().generic_string());
			stmtStr += escapedPath;
			sqlite3_free((void*)escapedPath);

			const std::vector<MetaDataDecl>& mdd = getMDDByType((*fit)->metadata.getType());
			for(unsigned int i = 0; i < mdd.size(); i++)
			{
				const char* escapedVal = sqlite3_mprintf("%Q", (*fit)->metadata.get(mdd.at(i).key).c_str());
				stmtStr += escapedVal;
				sqlite3_free((void*)escapedVal);

				if(i != mdd.size() - 1)
					stmtStr += ",";
			}
			stmtStr += ")";

			if(sqlite3_prepare(dbHandle, stmtStr.c_str(), -1, &statement, NULL) != SQLITE_OK || statement == NULL)
			{
				LOG(LogError) << "Error creating SQL write query \"" << stmtStr << "\"\n" << sqlite3_errmsg(dbHandle);
			}else{
				if(sqlite3_step(statement) != SQLITE_DONE)
				{
					LOG(LogError) << "Error performing SQL write \"" << stmtStr << "\"\n" << sqlite3_errmsg(dbHandle);
				}
				sqlite3_finalize(statement);
			}
			++fit;
		}

		sqlite3_exec(dbHandle, "COMMIT TRANSACTION", NULL, NULL, NULL);
	}else{
		LOG(LogError) << "Found no root folder for system \"" << system->getName() << "\"!";
	}

	sqlite3_close(dbHandle);
}

#endif // USE_SQLITE_GAMELIST