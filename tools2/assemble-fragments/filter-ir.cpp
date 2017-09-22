/* ===========================================================================
 *
 *                            PUBLIC DOMAIN NOTICE
 *               National Center for Biotechnology Information
 *
 *  This software/database is a "United States Government Work" under the
 *  terms of the United States Copyright Act.  It was written as part of
 *  the author's official duties as a United States Government employee and
 *  thus cannot be copyrighted.  This software/database is freely available
 *  to the public for use. The National Library of Medicine and the U.S.
 *  Government have not placed any restriction on its use or reproduction.
 *
 *  Although all reasonable efforts have been taken to ensure the accuracy
 *  and reliability of the software and data, the NLM and the U.S.
 *  Government do not and cannot warrant the performance or results that
 *  may be obtained by using this software or data. The NLM and the U.S.
 *  Government disclaim all warranties, express or implied, including
 *  warranties of performance, merchantability or fitness for any particular
 *  purpose.
 *
 *  Please cite the author in any work or product based on this material.
 *
 * ===========================================================================
 */

#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <string>
#include <stdexcept>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include "vdb.hpp"
#include "writer.hpp"

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "fragment.hpp"

void write(VDB::Writer const &out, unsigned const table, Fragment const &self) {
    for (auto && i : self.detail) {
        out.value(1 + (table - 1) * 8, self.group);
        out.value(2 + (table - 1) * 8, self.name);
        out.value(3 + (table - 1) * 8, int32_t(i.readNo));
        out.value(4 + (table - 1) * 8, std::string(i.sequence));
        if (i.aligned) {
            out.value(5 + (table - 1) * 8, i.reference);
            out.value(6 + (table - 1) * 8, i.strand);
            out.value(7 + (table - 1) * 8, int32_t(i.position));
            out.value(8 + (table - 1) * 8, i.cigar);
        }
        out.closeRow(table);
    }
}

static Fragment clean(Fragment &&raw) {
    auto &rslt = raw.detail;
    
    if (rslt.size() < 2)
        return raw;

    if (rslt.size() == 2) {
        if (rslt[1].readNo < rslt[0].readNo) {
            auto const tmp = rslt[1];
            rslt[1] = rslt[0];
            rslt[0] = tmp;
        }
        if (rslt[0].readNo != rslt[1].readNo)
            return raw;
    }
    else
        std::sort(rslt.begin(), rslt.end());
    
    return raw;
}

static void process(VDB::Writer const &out, Fragment const &fragment)
{
    auto reads = 0;
    auto firstRead = 0;
    auto lastRead = 0;
    auto aligned = 0;
    auto ambiguous = 0;

    for (auto && i : fragment.detail) {
        if (i.bad)
            goto DISCARD;
        if (i.aligned)
            ++aligned;
        if (i.sequence.ambiguous())
            ++ambiguous;
        if (reads == 0 || i.readNo != lastRead) {
            lastRead = i.readNo;
            if (reads == 0)
                firstRead = i.readNo;
            ++reads;
        }
    }
    if (aligned == 0) {
    DISCARD:
        write(out, 2, fragment);
        return;
    }

    if (fragment.detail.size() == reads) {
        write(out, aligned == reads ? 1 : 2, fragment);
        return;
    }

    auto detail = std::vector<Alignment>();
    {
        auto next = 0;
        while (next < fragment.detail.size()) {
            auto const first = next;
            while (next < fragment.detail.size() && fragment.detail[next].readNo == fragment.detail[first].readNo)
                ++next;
            
            auto const count = next - first;
            auto aligned = 0;
            auto ambiguous = 0;
            auto good = 0;
            auto firstGood = 0;
            for (auto i = first; i < next; ++i) {
                auto const &algn = fragment.detail[i];
                auto const a = algn.aligned;
                auto const b = algn.sequence.ambiguous();
                if (a) ++aligned;
                if (b) ++ambiguous;
                if (a & !b) {
                    if (good == 0) firstGood = i;
                    ++good;
                }
            }
            if (good == 0)
                goto DISCARD;

            if (count == 1) {
                detail.push_back(fragment.detail[first]);
                continue;
            }
            
            auto const &seq = fragment.detail[firstGood].sequence;
            detail.push_back(fragment.detail[firstGood]);
            for (auto i = first; i < next; ++i) {
                if (i == firstGood) continue;
                auto const &algn = fragment.detail[i];
                if (!algn.aligned) continue;
                if (ambiguous > 0 && algn.sequence.ambiguous()) {
                    detail.push_back(algn.truncated());
                    continue;
                }
                if (algn.sequence.isEquivalentTo(seq))
                    detail.push_back(algn);
                else
                    goto DISCARD;
            }
        }
    }
    write(out, 1, Fragment(fragment.group, fragment.name, detail));
    return;
}

static int process(VDB::Writer const &out, VDB::Database const &inDb)
{
    auto const in = Fragment::Cursor(inDb["RAW"]);
    auto const range = in.rowRange();
    auto const freq = (range.second - range.first) / 100.0;
    auto nextReport = 1;
    
    std::cerr << "info: processing " << (range.second - range.first) << " records" << std::endl;
    for (auto row = range.first; row < range.second; ) {
        auto const spot = clean(in.read(row, range.second));
        if (spot.detail.empty())
            continue;
        process(out, spot);
        if (nextReport * freq <= (row - range.first)) {
            std::cerr << "prog: processed " << nextReport << "%" << std::endl;
            ++nextReport;
        }
    }
    std::cerr << "prog: Done" << std::endl;

    return 0;
}

static int process(std::string const &irdb, std::ostream &out)
{
    auto const writer = VDB::Writer(out);
    
    writer.destination("IR.vdb");
    writer.schema("aligned-ir.schema.text", "NCBI:db:IR:raw");
    writer.info("reorder-ir", "1.0.0");
    
    writer.openTable(1, "RAW");
    writer.openColumn(1, 1, 8, "READ_GROUP");
    writer.openColumn(2, 1, 8, "FRAGMENT");
    writer.openColumn(3, 1, 32, "READNO");
    writer.openColumn(4, 1, 8, "SEQUENCE");
    writer.openColumn(5, 1, 8, "REFERENCE");
    writer.openColumn(6, 1, 8, "STRAND");
    writer.openColumn(7, 1, 32, "POSITION");
    writer.openColumn(8, 1, 8, "CIGAR");
    
    writer.openTable(2, "DISCARDED");
    writer.openColumn(1 + 8, 2, 8, "READ_GROUP");
    writer.openColumn(2 + 8, 2, 8, "FRAGMENT");
    writer.openColumn(3 + 8, 2, 32, "READNO");
    writer.openColumn(4 + 8, 2, 8, "SEQUENCE");
    writer.openColumn(5 + 8, 2, 8, "REFERENCE");
    writer.openColumn(6 + 8, 2, 8, "STRAND");
    writer.openColumn(7 + 8, 2, 32, "POSITION");
    writer.openColumn(8 + 8, 2, 8, "CIGAR");
    
    writer.beginWriting();

    writer.defaultValue<char>(5, 0, 0);
    writer.defaultValue<char>(6, 0, 0);
    writer.defaultValue<int32_t>(7, 0, 0);
    writer.defaultValue<char>(8, 0, 0);
    
    writer.defaultValue<char>(5 + 8, 0, 0);
    writer.defaultValue<char>(6 + 8, 0, 0);
    writer.defaultValue<int32_t>(7 + 8, 0, 0);
    writer.defaultValue<char>(8 + 8, 0, 0);
    
    auto const mgr = VDB::Manager();
    auto const result = process(writer, mgr[irdb]);
    
    writer.endWriting();
    
    return result;
}

using namespace utility;
namespace filterIR {
    static void usage(std::string const &program, bool error) {
        (error ? std::cerr : std::cout) << "usage: " << program << " [-out=<path>] <ir db>" << std::endl;
        exit(error ? 3 : 0);
    }
    
    static int main(CommandLine const &commandLine) {
        CIGAR::test();
        
        for (auto && arg : commandLine.argument) {
            if (arg == "-help" || arg == "-h" || arg == "-?") {
                usage(commandLine.program, false);
            }
        }
        auto out = std::string();
        auto run = std::string();
        for (auto && arg : commandLine.argument) {
            if (arg.substr(0, 5) == "-out=") {
                out = arg.substr(5);
                continue;
            }
            if (run.empty()) {
                run = arg;
                continue;
            }
            usage(commandLine.program, true);
        }
        if (run.empty()) {
            usage(commandLine.program, true);
        }
        if (out.empty())
            return process(run, std::cout);

        auto ofs = std::ofstream(out);
        if (ofs.bad()) {
            std::cerr << "failed to open output file: " << out << std::endl;
            exit(3);
        }
        return process(run, ofs);
    }
}

int main(int argc, char *argv[])
{
    return filterIR::main(CommandLine(argc, argv));
}
