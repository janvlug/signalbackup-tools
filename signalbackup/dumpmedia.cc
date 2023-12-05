/*
  Copyright (C) 2021-2023  Selwin van Dijk

  This file is part of signalbackup-tools.

  signalbackup-tools is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  signalbackup-tools is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with signalbackup-tools.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "signalbackup.ih"

bool SignalBackup::dumpMedia(std::string const &dir, std::vector<std::string> const &daterangelist,
                             std::vector<long long int> const &threads, bool overwrite) const
{
  Logger::message("Dumping media to dir '", dir, "'");

  if (!d_database.containsTable("part") || !d_database.tableContainsColumn("part", "display_order"))
  {
    Logger::error("Database too badly damaged or too old, dumping media is not (yet) supported, consider a full decrypt by just passing a directory as output");
    return false;
  }

  // check if dir exists, create if not
  if (!prepareOutputDirectory(dir, overwrite))
    return false;

  MimeTypes mimetypes;
  std::pair<std::vector<int>, std::vector<std::string>> conversations; // links thread_id to thread title, if the
                                                                       // folder already exists, but from another _id,
                                                                       // it is a different thread with the same name

#if __cplusplus > 201703L
  for (int count = 0; auto const &aframe : d_attachments)
#else
  int count = 0;
  for (auto const &aframe : d_attachments)
#endif
  {

    ++count;
    Logger::message_overwrite("Saving attachments...  ", count, "/", d_attachments.size());

    AttachmentFrame *a = aframe.second.get();

    //std::cout << "Looking for attachment: " << std::endl;
    //std::cout << "rid: " << a->rowId() << std::endl;
    //std::cout << "uid: " << a->attachmentId() << std::endl;

    SqliteDB::QueryResults results;

    // minimal query, for incomplete database
    bool fullbackup = false;
    std::string query = "SELECT part.mid, part.ct, part.file_name, part.display_order FROM part WHERE part._id == ? AND part.unique_id == ?";
    // if all tables for detailed info are present...
    if (d_database.containsTable(d_mms_table) && d_database.containsTable("thread") &&
        d_database.containsTable("groups") && d_database.containsTable("recipient"))
    {
      fullbackup = true;
      query = "SELECT part.mid, part.ct, part.file_name, part.display_order, " +
        d_mms_table + ".date_received, " + d_mms_table + "." + d_mms_type + ", " +
        d_mms_table + ".thread_id, thread." + d_thread_recipient_id +
        ", COALESCE(groups.title,recipient." + d_recipient_system_joined_name + ", recipient.profile_joined_name, "
        "recipient." + d_recipient_profile_given_name + ")"
        " AS 'chatpartner' FROM part "
        "LEFT JOIN " + d_mms_table + " ON part.mid == " + d_mms_table + "._id "
        "LEFT JOIN thread ON " + d_mms_table + ".thread_id == thread._id "
        "LEFT JOIN recipient ON thread." + d_thread_recipient_id + " == recipient._id "
        "LEFT JOIN groups ON recipient.group_id == groups.group_id "
        "WHERE part._id == ? AND part.unique_id == ?";
    }

    if (!threads.empty())
    {
      query += " AND thread._id IN (";
      for (uint i = 0; i < threads.size(); ++i)
        query += bepaald::toString(threads[i]) + ((i == threads.size() - 1) ? ")" : ",");
    }

    if (!daterangelist.empty())
    {
      // create dateranges
      std::vector<std::pair<std::string, std::string>> dateranges;
      if (daterangelist.size() % 2 == 0)
        for (uint i = 0; i < daterangelist.size(); i += 2)
          dateranges.push_back({daterangelist[i], daterangelist[i + 1]});

      std::string datewhereclause;

      for (uint i = 0; i < dateranges.size(); ++i)
      {
        bool needrounding = false;
        long long int startrange = dateToMSecsSinceEpoch(dateranges[i].first);
        long long int endrange   = dateToMSecsSinceEpoch(dateranges[i].second, &needrounding);
        if (startrange == -1 || endrange == -1 || endrange < startrange)
        {
          Logger::error("Skipping range: '", dateranges[i].first, " - ", dateranges[i].second, "'. Failed to parse or invalid range.");
          Logger::error_indent(startrange, " ", endrange);
          continue;
        }

        if (d_verbose) [[unlikely]]
          Logger::message("  Using range: ", dateranges[i].first, " - ", dateranges[i].second, " (", startrange, " - ", endrange, ")");

        if (needrounding)// if called with "YYYY-MM-DD HH:MM:SS"
          endrange += 999; // to get everything in the second specified...

        dateranges[i].first = bepaald::toString(startrange);
        dateranges[i].second = bepaald::toString(endrange);

        datewhereclause += (datewhereclause.empty() ? " AND (" : " OR ") + "date_received BETWEEN "s + dateranges[i].first + " AND " + dateranges[i].second;
        if (i == dateranges.size() - 1)
          datewhereclause += ')';
      }

      query += datewhereclause;
    }

    if (d_verbose) [[unlikely]]
      Logger::message("Dump media query: ", query);

    if (!d_database.exec(query, {static_cast<long long int>(a->rowId()), static_cast<long long int>(a->attachmentId())},  &results))
      return false;

    //results.prettyPrint();

    if (results.rows() == 0 && (!threads.empty() || !daterangelist.empty())) // probably an attachment for a de-selected thread
      continue;

    if (results.rows() != 1)
    {
      Logger::error("Unexpected number of results: ", results.rows(), " (rowid: ", a->rowId(), ", uniqueid: ", a->attachmentId(), ")");
      continue;
    }

    std::string filename;
    long long int datum = a->attachmentId();

    if (fullbackup && !results.isNull(0, "date_received"))
      datum = results.getValueAs<long long int>(0, "date_received");
    long long int order = results.getValueAs<long long int>(0, "display_order");

    if (!results.isNull(0, "file_name")) // file name IS SET in database
      filename = sanitizeFilename(results.valueAsString(0, "file_name"));

    if (filename.empty()) // filename was not set in database or was not impossible
    {                     // to sanitize (eg reserved name in windows 'COM1')
      // get datestring
      std::time_t epoch = datum / 1000;
      std::ostringstream tmp;
      tmp << std::put_time(std::localtime(&epoch), "signal-%Y-%m-%d-%H%M%S");
      //tmp << "." << datum % 1000;

      // get file ext
      std::string mime = results.valueAsString(0, "ct");
      std::string ext = std::string(mimetypes.getExtension(mime));
      if (ext.empty())
      {
        ext = "attach";
        Logger::warning("mimetype not found in database (", mime, ") -> saving as '", tmp.str(), ".", ext, "'");
      }

      //build filename
      filename = tmp.str() + ((order) ? ("_" + bepaald::toString(order)) : "") + "." + ext;
    }

    // std::cout << "FILENAME: " << filename << std::endl;
    std::string targetdir = dir;
    if (fullbackup && !results.isNull(0, "thread_id") && !results.isNull(0, "chatpartner")
        && !results.isNull(0, d_mms_type))
    {
      long long int tid = results.getValueAs<long long int>(0, "thread_id");
      std::string chatpartner = sanitizeFilename(results.valueAsString(0, "chatpartner"));
      if (chatpartner.empty())
        chatpartner = "Contact " + bepaald::toString(tid);

      int idx_of_thread = -1;
      if ((idx_of_thread = bepaald::findIdxOf(conversations.first, tid)) == -1) // idx not found
      {
        if (std::find(conversations.second.begin(), conversations.second.end(), chatpartner) == conversations.second.end()) // chatpartner not used yet
        {
          // add it
          conversations.first.push_back(tid);
          conversations.second.push_back(chatpartner);
          idx_of_thread = conversations.second.size() - 1;
        }
        else // new conversation, but another conversation with same name already exists!
        {
          // get unique conversation name
          chatpartner += "(2)";
          while (std::find(conversations.second.begin(), conversations.second.end(), chatpartner) != conversations.second.end())
            chatpartner += "(2)";
          conversations.first.push_back(tid);
          conversations.second.push_back(chatpartner);
          idx_of_thread = conversations.second.size() - 1;
        }
      } // else, thread was found, use the name that was used before

      // create dir if not exists
      if (!bepaald::isDir(dir + "/" + conversations.second[idx_of_thread]))
      {
        // std::cout << " Creating subdirectory '" << conversations.second[idx_of_thread] << "' for conversation..." << std::endl;
        if (!bepaald::createDir(dir + "/" + conversations.second[idx_of_thread]))
        {
          //std::cout << " ERROR creating directory '" << dir << "/" << conversations.second[idx_of_thread] << "'" << std::endl;
          Logger::error("Failed to create directory '", dir, "/", conversations.second[idx_of_thread], "'");
          continue;
        }
      }

      long long int msg_box = results.getValueAs<long long int>(0, d_mms_type);
      targetdir = dir + "/" + conversations.second[idx_of_thread] + "/" + (Types::isOutgoing(msg_box) ? "sent" : "received");

      // create dir if not exists
      if (!bepaald::isDir(targetdir))
      {
        //std::cout << " Creating subdirectory '" << targetdir << "' for conversation..." << std::endl;
        if (!bepaald::createDir(targetdir))
        {
          Logger::error("Failed to create directory '", targetdir, "'");
          continue;
        }
      }
    }

    // make filename unique
    if (!makeFilenameUnique(targetdir, &filename))
    {
      Logger::error("getting unique filename for '", targetdir, "/", filename, "'");
      continue;
    }
    /*
    while (bepaald::fileOrDirExists(targetdir + "/" + filename))
    {
      //std::cout << std::endl << "File exists: " << targetdir << "/" << filename << " -> ";

      std::filesystem::path p(filename);
      std::regex numberedfile(".*( \\(([0-9]*)\\))$");
      std::smatch sm;
      std::string filestem(p.stem().string());
      std::string ext(p.extension().string());
      int counter = 2;
      if (regex_match(filestem, sm, numberedfile) && sm.size() >= 3 && sm[2].matched)
      {
        // increase the counter
        counter = bepaald::toNumber<int>(sm[2]) + 1;
        // remove " (xx)" part from stem
        filestem.erase(sm[1].first, sm[1].second);
      }
      filename = filestem + " (" + bepaald::toString(counter) + ")" + p.extension().string();

      //std::cout << filename << std::endl;
    }
    */
    std::ofstream attachmentstream(targetdir + "/" + filename, std::ios_base::binary);

    if (!attachmentstream.is_open())
    {
      Logger::error("Failed to open file for writing: '", targetdir, "/", filename, "'");
      continue;
    }
    else
      if (!attachmentstream.write(reinterpret_cast<char *>(a->attachmentData()), a->attachmentSize()))
      {
        Logger::error("Failed to write data to file: '", targetdir, "/", filename, "'");
        a->clearData();
        continue;
      }
    attachmentstream.close(); // need to close, or the auto-close will change files mtime again.
    a->clearData();

    setFileTimeStamp(targetdir + "/" + filename, datum); // ignoring return for now...

    // !! ifdef c++20
    //std::error_code ec;
    //std::filesystem::last_write_time(dir + "/" + chatpartner + "/" + filename, std::chrono::clock_cast<std::filesystem::file_time_type>(datum / 1000), ec);
  }
  Logger::message("done.");
  return true;
}
