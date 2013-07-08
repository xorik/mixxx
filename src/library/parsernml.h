#ifndef PARSERNML_H
#define PARSERNML_H

#include "parser.h"
#include "library/basesqltablemodel.h"


// Traktor playlist file export
class ParserNml : public Parser
{
public:
    ParserNml();
    ~ParserNml();

    static bool writeNMLFile(const QString &file_str, BaseSqlTableModel* pPlaylistTableModel);
};

#endif // PARSERNML_H
