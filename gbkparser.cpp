#include "gbkparser.h"

#include "database.h"
#include "structures.h"

#include <QDebug>
#include <QFileInfo>
#include <QStringList>
#include <QThread>
// #include <QSqlQuery>

// #include <QByteArray>
// #include <QCoreApplication>
// #include <QFile>
// #include <QSqlDatabase>
// #include <QSqlError>
// #include <QSqlField>
// #include <QSqlQuery>
// #include <QSqlRecord>
// #include <QThread>

void GbkParser::setSource(QIODevice *sourceStream, const QString &fileName)
{
    _io = sourceStream;
    _stream = new QTextStream(_io);
    _stream->setCodec("UTF-8");
    _state = State::TopLevel;
    _fileName = QFileInfo(fileName).fileName();
}

void GbkParser::setDatabase(QSharedPointer<Database> db)
{
    _db = db;
}

void GbkParser::setOverrideOrganismName(const QString &name)
{
    _overrideOrganismName = name;
}

bool GbkParser::atEnd() const
{
    return !_io || !_stream || _stream->atEnd();
}

SequencePtr GbkParser::readSequence()
{
    // qDebug() << "parsing sequence";
    _state = TopLevel;
    SequencePtr seq(new Sequence);
    seq->sourceFileName = _fileName;
    QString topLevelName;
    QString topLevelValue;
    QString secondLevelName;
    QString secondLevelValue;
    while (!atEnd()) {
        QString currentLine = _stream->readLine();
        _currentLineNo += 1;
        currentLine.replace('\t', "    ");
        if ("//" == currentLine.trimmed()) {
            break;
        }
        if (State::TopLevel == _state) {
            const QString prefix =
                    currentLine.length() > 12
                    ? currentLine.left(12).trimmed()
                    : currentLine.trimmed();

            const QString value =
                    currentLine.length() > 12
                    ? currentLine.mid(12).trimmed()
                    : QString();

            if (prefix.isEmpty()) {
                if (topLevelValue.length() > 0) {
                    topLevelValue.push_back('\n');
                }
                topLevelValue += value;
            }
            else {
                if (topLevelName.length() > 0) {
                    parseTopLevel(topLevelName, topLevelValue, seq);
                }
                if (State::Features == _state) {
                    secondLevelName = prefix;
                    secondLevelValue = value;
                }
                else {
                    topLevelName = prefix;
                    topLevelValue = value;
                }
            }
        }
        else if (State::Features == _state) {
            const QString prefix =
                    currentLine.length() > 21
                    ? currentLine.left(21).trimmed()
                    : currentLine.trimmed();
            const QString value =
                    currentLine.length() > 21
                    ? currentLine.mid(21).trimmed()
                    : QString();

            if (prefix.isEmpty()) {
                if (secondLevelValue.length() > 0) {
                    secondLevelValue.push_back('\n');
                }
                secondLevelValue += value;
            }
            else {
                if (secondLevelName.length() > 0) {
                    // qDebug() << "start parsing second";
                    parseSecondLevel(secondLevelName, secondLevelValue, seq);
                    // qDebug() << "finish parsing second";
                }
                secondLevelName = prefix;
                secondLevelValue = value;
                _featureStartLineNo = _currentLineNo;
            }
            if ("ORIGIN" == prefix) {
                _state = State::Origin;
            }
        }
        else if (State::Origin == _state) {
            QString value =
                    currentLine.length() > 10
                    ? currentLine.mid(10)
                    : QString();
            value.replace(' ', "");
            value = value.toUpper();
            seq->origin.append(value.toLatin1());
        }
    }
    if (seq->genes.isEmpty() && seq->description.isEmpty()) {
        seq.clear();
    }else {
        // qDebug() << "making real";
        makeRealExons(seq);
        // qDebug() << "filling introns& exons";
        fillIntronsAndExonsFromOrigin(seq);
        // qDebug() << "check main error";
        checkIsoformsMainErrors(seq);
        // qDebug() << "finish checking";
    }

    return seq;
}

GenePtr GbkParser::findGeneMatchingLocation(
        const QList<GenePtr> &genes,
        const quint32 start, const quint32 end,
        const bool backwardChain)
{
    Q_FOREACH(GenePtr gene, genes) {
        const bool startMatch = start >= gene->start;
        const bool endMatch = end <= gene->end;
        const bool chainMatch = backwardChain == gene->backwardChain;
        if (startMatch && endMatch && chainMatch) {
            return gene;
        }
    }

    return GenePtr();
}

GenePtr GbkParser::findGeneContainingLocation(
        const QList<GenePtr> &genes,
        const quint32 start, const quint32 end,
        const bool backwardChain)
{
    Q_FOREACH(GenePtr gene, genes) {
        const bool startMatch = start >= gene->start;
        const bool endMatch = end <= gene->end;
        const bool chainMatch = backwardChain == gene->backwardChain;
        if (startMatch && endMatch && chainMatch) {
            return gene;
        }
    }

    return GenePtr();
}

bool cdsRangesMatchesRnaRanges(const QList<Range> & cdsRanges,
                               const QList<Range> & mrnaRanges)
{
    // CDS corresponds to mRNA ⇔ :
    //  1. First exocC[xc,yc] ∊ CDS: (∃ exonM[xm,ym] : xc >= xm && yc == ym)
    //  2. ∀ inner exonC ∊ CDS: (∃ exonM ∊ mRNA: exonC == exonM)
    //  3. Last exonC[xc,yx] ∊ CDS: (∃ exonM[xm,ym] : xc == xm && yc <= ym)
    QVector<bool> cdsRangesGood(cdsRanges.size(), false);
    for (int i=0; i<cdsRanges.size(); ++i) {
        const bool first = 0 == i;
        const bool last  = cdsRanges.size()-1 == i;
        const bool mid = !first && !last;
        const bool single = cdsRanges.size() == 1;

        const bool leftBoundMustExactMatch  = (!single) && (mid || last);
        const bool rightBoundMustExactMatch = (!single) && (mid || first);

        const Range & cds = cdsRanges.at(i);

        for (int j=0; j<mrnaRanges.size(); ++j) {
            const Range & mrna = mrnaRanges.at(j);
            bool leftOk = leftBoundMustExactMatch
                    ? mrna.start == cds.start
                    : mrna.start <= cds.start;
            bool rightOk = rightBoundMustExactMatch
                    ? mrna.end == cds.end
                    : mrna.end >= cds.end;
            if (leftOk && rightOk) {
                cdsRangesGood[i] = true;
                break;
            }
        }
    }
    return cdsRangesGood.count(true) == cdsRangesGood.size();
}

IsoformPtr GbkParser::findRnaIsoformContainingLocation(
        const QList<IsoformPtr> &isoforms,
        const QList<quint32> & starts,
        const QList<quint32> & ends,
        const bool backwardChain)
{    
    const QList<Range> ranges = Range::createList(starts, ends);

    Q_FOREACH(IsoformPtr iso, isoforms) {
        if (Isoform::MRNA == iso->type) {
            const bool chainMatch = backwardChain == iso->gene.toStrongRef()->backwardChain;
            if (chainMatch && cdsRangesMatchesRnaRanges(ranges, iso->mRnaRanges)) {
                return iso;
            }
        }
    }    
    return IsoformPtr();
}

void GbkParser::parseTopLevel(const QString &prefix, QString value, SequencePtr seq)
{
    //LOCUS       NT_008705           39626682 bp    DNA     linear   CON 12-MAR-2015
    if ("LOCUS" == prefix) {
        const QStringList words = value.split(QRegExp("\\s+"));
        seq->refSeqId = words[0];
        seq->length = words[1].toUInt();
        seq->gbk_date.setDate(1, 1, 1);
        static QRegExp rxDate("(\\d+)-(\\S+)-(\\d\\d\\d\\d)");
        int position = rxDate.indexIn(words[words.size() - 1]);
        if (-1 != position) {
            int day = rxDate.cap(1).toInt();
            int year = rxDate.cap(3).toInt();
            const QString monthString = rxDate.cap(2).toLower().left(3);
            static const QStringList Months = QStringList()
                    << "jan" << "feb" << "mar"
                    << "apr" << "may" << "jun"
                    << "jul" << "aug" << "sep"
                    << "oct" << "nov" << "dec";
            int month = 1 + Months.indexOf(monthString);
            const bool dayValid = 1 <= day && day <= 31;
            const bool monthValid = 1 <= month && month <= 12;
            const bool yearValid = 1970 <= year && year <= 2039;
            if (dayValid && monthValid && yearValid) {
                seq->gbk_date.setDate(year, month, day);
            }else{
                seq->gbk_date.setDate(2, 2, 2);
                qDebug() << "DATA problems: " << year << "-" << month << "-" << day;        
            }
        }else{
            seq->gbk_date.setDate(1, 1, 1);
        }
        qDebug() << "... " << seq->refSeqId
                 << " from " << _fileName
                 << " by worker " << QThread::currentThreadId();
    }
    //ORGANISM  Homo sapiens
    else if ("ORGANISM" == prefix) {
        const QStringList lines = value.split('\n', QString::SkipEmptyParts);
        const QString name = _overrideOrganismName.isEmpty()
                ? lines[0].trimmed()
                : _overrideOrganismName;
        // qDebug() << "parse organism";
        seq->organism = _db->findOrCreateOrganism(name).toWeakRef();
        if (seq->organism.toStrongRef()->taxonomyList.size() == 0) {
            for (int i=1; i<lines.size(); ++i) {
                const QStringList words = lines[i].split(';', QString::SkipEmptyParts);
                Q_FOREACH (QString word, words) {
                    word.replace('.', "");
                    word = word.simplified();
                    seq->organism.toStrongRef()->taxonomyList.append(word);
                }
            }
        }
    }
    //DEFINITION  Homo sapiens chromosome 10 genomic scaffold, GRCh38.p2 Primary
    else if ("DEFINITION" == prefix) {
        seq->description = value.replace('\n', ' ').simplified();
    }
    //VERSION     NT_008705.17  GI:568815281
    else if ("VERSION" == prefix) {
        seq->version = value.replace('\n', ' ').simplified();
    }
    //FEATURES             Location/Qualifiers
    else if ("FEATURES" == prefix) {
        _state = State::Features;
        _featureStartLineNo = _currentLineNo;
    }
    //ORIGIN      
    else if ("ORIGIN" == prefix) {
        _state = State::Origin;
    }

    // TODO interact with organisms records
}

void GbkParser::parseSecondLevel(const QString &prefix, QString value, SequencePtr seq)
{
    if ("ORIGIN" == prefix) {
        _state = State::Origin;
    }
    else if ("gene" == prefix) {
        // qDebug() << "in gene";
        seq->genes.append(parseGene(value, seq));
        // qDebug() << "out gene";
    }
    else if ("source" == prefix) {
        // qDebug() << "in source";
        const auto attrs = parseFeatureAttributes(value);
        if (attrs.contains("organelle")) {
            seq->organism.toStrongRef()->dbMitochondria =
                    "mitochondrion" == attrs["organelle"];
        }
        if (attrs.contains("db_xref")) {
            seq->organism.toStrongRef()->taxonomyXref =
                    attrs["db_xref"];
        }
        if (attrs.contains("organism")) {
            //Q_ASSERT(seq->organism.toStrongRef()->name == attrs["organism"]);
        }
        if (attrs.contains("chromosome")) {
            seq->chromosome =
                    _db->findOrCreateChromosome(
                        attrs["chromosome"],
                        seq->organism.toStrongRef()
                    );
        }else if (attrs.contains("organelle") && "mitochondrion" == attrs["organelle"]) {
            seq->chromosome =
                    _db->findOrCreateChromosome("mitochondrion",
                                                seq->organism.toStrongRef());
        }else if(seq->sourceFileName.contains("chr")){
            QRegExp rx("(chr)(.*)(.gbk)");
            rx.indexIn(seq->sourceFileName);
            QStringList list = rx.capturedTexts();
            seq->chromosome =
                    _db->findOrCreateChromosome(list[2],
                                                seq->organism.toStrongRef());

        }else{
            seq->chromosome =
                    _db->findOrCreateChromosome("undefined",
                                                seq->organism.toStrongRef());

        }
        // qDebug() << "out source";
    }
    else if ("CDS" == prefix || prefix.endsWith("RNA")) {
        // qDebug() << "in cds/rna";
        parseCdsOrRna(prefix, value, seq);
        // qDebug() << "out cds/rna";
    }
}

GenePtr GbkParser::parseGene(const QString & value, SequencePtr seq)
{
    GenePtr gene(new Gene);
    parseRange(value, &gene->start, &gene->end, &gene->backwardChain, 0, 0);
    gene->sequence = seq.toWeakRef();
    const auto attrs = parseFeatureAttributes(value);
    if (attrs.contains("gene")) {
        gene->name = attrs["gene"];
    }
    if (attrs.contains("db_xref")) {
        QStringList geneID = attrs["db_xref"].split("\n").filter(QRegExp("^GeneID:*"));
        if (geneID.length()>0){
            gene->ncbiGeneId = geneID[0].split(":")[1];
        }   
    }
    gene->isPseudoGene = attrs.contains("pseudo") || attrs.contains("pseudogene");
    if (seq->chromosome && seq->chromosome.toStrongRef()->name.toLower().startsWith("unk")) {
        OrganismPtr organism = seq->organism.toStrongRef();
        organism->mutex.lock();
        organism->unknownProtGenesCount++;
        organism->mutex.unlock();
    }
    return gene;
}

void GbkParser::parseCdsOrRna(const QString & prefix,
                              const QString &value, SequencePtr seq)
{    
    const auto attrs = parseFeatureAttributes(value);
    quint32 start = UINT32_MAX;
    quint32 end = 0;
    bool bw = false;
    QList<quint32> starts;
    QList<quint32> ends;
    parseRange(value, &start, &end, &bw, &starts, &ends);
    const QList<GenePtr> & allGenes = seq->genes;

    QRegExp gene_id_reg = QRegExp("^GeneID:*");
    QRegExp gi_reg = QRegExp("^GI:*");
    GenePtr targetGene;
    IsoformPtr targetIsoform;
    OrganismPtr organism = seq->organism.toStrongRef();

    if ("CDS" == prefix) {
        // CDS might have non-coding bounds inside gene
        targetGene = findGeneContainingLocation(allGenes, start, end, bw);
        const QString & refSeqId = seq->refSeqId;
        const QString dbXref = attrs.contains("db_xref") ? attrs["db_xref"] : QString();
        const QString product = attrs.contains("product") ? attrs["product"] : QString();
        if (! targetGene) {
            _db->addOrphanedCDS(seq->sourceFileName, _featureStartLineNo, _currentLineNo,
                                refSeqId, dbXref, product);
            return;
        }

        // CDS must be linked to existing mRNA isoform
        const QList<IsoformPtr> & geneIsoforms = targetGene->isoforms;
        targetIsoform = findRnaIsoformContainingLocation(
                        geneIsoforms, starts, ends, bw
                    );

        if (! targetIsoform) {
           // const QString protName = attrs.contains("protein_id")
           //         ? attrs["protein_id"] : "[unknown_protein_id]";
           // const QString seqFileName = seq->sourceFileName;
           // const QString message =
           //         QString("Can't find mRNA for CDS: { protein = %1, sequenceFile = %2 }")
           //         .arg(protName).arg(seqFileName);
           // qWarning() << message;
            _db->addOrphanedCDS(seq->sourceFileName, _featureStartLineNo, _currentLineNo,
                                refSeqId, dbXref, product);
            return;
        }

        if (Isoform::CDS == targetIsoform->type) {
            // There is existing CDS, so clone it as new isoform
            targetIsoform = IsoformPtr(new Isoform(*targetIsoform.data()));
            targetGene->isoforms.push_back(targetIsoform);
            targetIsoform->exons.clear();
            targetIsoform->introns.clear();
        }

        targetIsoform->type = Isoform::CDS;
        targetGene->hasCDS = true;
        organism->mutex.lock();
        organism->cdsCount ++;
        if (seq->chromosome && seq->chromosome.toStrongRef()->name.toLower().startsWith("unk")) {
            organism->unknownProtCdsCount ++;
        }
        organism->mutex.unlock();

        targetIsoform->cdsStart = start;
        targetIsoform->cdsEnd = end;
        targetIsoform->exonsCdsCount = starts.size();
        targetGene->isProteinButNotRna = true;
        targetGene->startCode = start;
        targetGene->endCode = end;
    }
    else {
        // *RNA range must be equal to gene location
        targetGene = findGeneMatchingLocation(allGenes, start, end, bw);

        if (! targetGene) {
            return;
        }
        if ("mRNA" == prefix) {
            targetIsoform = IsoformPtr(new Isoform);
            targetIsoform->type = Isoform::MRNA;
            targetIsoform->mrnaStart = start;
            targetIsoform->mrnaEnd = end;
            targetIsoform->exonsMrnaCount = starts.size();
            targetIsoform->mRnaRanges = Range::createList(starts, ends);
            targetGene->isoforms.push_back(targetIsoform);            
        }
        else {
            targetGene->hasRNA = true;
            organism->mutex.lock();
            organism->rnaCount ++;
            organism->mutex.unlock();
        }
    }

    if (! targetIsoform) {
        return;
    }

    targetIsoform->gene = targetGene.toWeakRef();
    targetIsoform->sequence = targetGene->sequence;

    if (attrs.contains("protein_id")) {
        targetIsoform->proteinId = attrs["protein_id"];
    }
    if (attrs.contains("db_xref")) {
        if (targetGene->ncbiGeneId.isNull()){
            QStringList geneID = attrs["db_xref"].split("\n").filter(gene_id_reg); 
            if (geneID.length()>0){
                targetGene->ncbiGeneId = geneID[0].split(":")[1];
            }
        }
        if (targetIsoform->proteinXref.isNull()){
            QStringList gis = attrs["db_xref"].split("\n").filter(gi_reg);
            if (gis.length() > 0){
                targetIsoform->proteinXref = gis[0];
            }else{
                targetIsoform->proteinXref = attrs["db_xref"];
            }
        }
    }
    if (attrs.contains("product")) {
        targetIsoform->product = attrs["product"];
    }
    if (attrs.contains("note")) {
        targetIsoform->note = attrs["note"];
    }

    if ("CDS" == prefix) {
        // qDebug() << attrs.keys();
        // if (attrs.contains("codon_start")){
        //     qDebug() << attrs["codon_start"];
        // };
        createIntronsAndExons(targetIsoform,
                              false,
                              bw,
                              starts, ends,
                              attrs);

        if (attrs.contains("translation")) {
            targetIsoform->translation = attrs["translation"];
        }

    }
}

void GbkParser::createIntronsAndExons(IsoformPtr isoform,
                                      bool rna, bool bw,
                                      const QList<quint32> &starts,
                                      const QList<quint32> ends,
                                      const QMap<QString,QString> attrs)
{
    Q_ASSERT(starts.size() == ends.size());
    if (starts.size() == 0) {
        return;
    }

    int startIndex = bw ? starts.size() - 1 : 0;
    int endIndex = bw ? -1 : starts.size();
    int increment = bw ? -1 : 1;

    quint8 phase = 0;

    for (int exonIndex = startIndex;
         exonIndex != endIndex;
         exonIndex += increment)
    {
        int start = starts[exonIndex];
        if (attrs.contains("codon_start")) {
            start += attrs["codon_start"].toInt()-1;
        }
        const int end = ends[exonIndex];
        ExonPtr exon(new Exon);
        if (start > end){
            exon->start = end;
            exon->lengthPhase = 0;
            phase = exon->endPhase = 0;
            exon->startPhase = 0;
            exon->stash = true;
        }else{
            exon->start = start;
            exon->lengthPhase = (exon->end - exon->start + 1) % 3;
            phase = exon->endPhase = (phase + end - start + 1) % 3;
            exon->startPhase = phase;
        }
        
        exon->end = end;
        exon->isoform = isoform;
        exon->gene = isoform->gene;
        exon->sequence = isoform->sequence;
        isoform->exons.push_back(exon);
    }

    if (1 == isoform->exons.size()) {
        ExonPtr exon = isoform->exons.first();
        exon->index = exon->revIndex = 0;
        exon->type = Exon::Type::OneExon;
    }
    else {
        for (int index = 0; index < isoform->exons.size(); ++index) {
            ExonPtr exon = isoform->exons.at(index);
            exon->index = index;
            exon->revIndex = isoform->exons.size() - index - 1;
            if (0 == index) {
                exon->type = Exon::Type::Start;
            }
            else if (isoform->exons.size()-1 == index) {
                exon->type = Exon::Type::End;
                if (exon->end == exon->start){
                    isoform->errorMain = true;
                }
            }
            else {
                exon->type = Exon::Type::Inner;
            }

            if (index > 0) {
                ExonPtr prevExon = isoform->exons[index-1];
                IntronPtr intron(new Intron);
                intron->isoform = isoform;
                intron->gene = isoform->gene;
                intron->sequence = isoform->sequence;
                intron->prevExon = prevExon;
                intron->nextExon = exon;
                intron->start = bw ? exon->end + 1 : prevExon->end + 1;
                intron->end = bw ? prevExon->start - 1 : exon->start - 1;
                intron->index = index - 1;
                intron->revIndex = isoform->exons.size() - index - 1;
                intron->phase = prevExon->endPhase;
                intron->lengthPhase = (intron->end - intron->start + 1) % 3;
                const quint8 prevStartPhase = prevExon->startPhase;
                const quint8 intrStartPhase = prevExon->endPhase;
                const quint8 nextEndPhase = exon->endPhase;
                const size_t typeIndex =
                        1 +  // SQL id's starts from 1 but not 0
                        9 * prevStartPhase +  // use prev start phase as group number
                        3 * intrStartPhase +  // use intron phase as row number
                        nextEndPhase;  // use next end phase as column number
                intron->intronTypeId = typeIndex;
                isoform->introns.push_back(intron);
                prevExon->nextIntron = intron;
                exon->prevIntron = intron;
            }
        }
    }
    GenePtr gene = isoform->gene.toStrongRef();

    gene->maxIntronsCount =
            qMax(gene->maxIntronsCount, quint32(isoform->introns.size()));

    if (rna) {
        gene->isProteinButNotRna = false;
        isoform->exonsMrnaCount = isoform->exons.size();
    }
    else {
        isoform->exonsCdsCount = isoform->exons.size();
    }

    Q_FOREACH(IsoformPtr iso, gene->isoforms) {
        if (quint32(iso->introns.size()) == gene->maxIntronsCount){
            iso->isMaximumByIntrons = gene->maxIntronsCount;
            Q_FOREACH(ExonPtr exon, iso->exons){
                exon-> fromMainIsoform = 1;
            }
            Q_FOREACH(IntronPtr intron, iso->introns){
                intron-> fromMainIsoform = 1;
            }
        }
        
                
    }

}

QByteArray GbkParser::dnaReverseComplement(const QByteArray &origin,
                                           int start, int end)
{
    if (end > start) {
        // ensure reverse indexing
        int t = start;
        start = end;
        end = t;
    }
    const int length = start - end + 1;  // inclusive both bounds
    QByteArray result(length, '?');
    for (int i=0; i<length; ++i) {
        int originIndex = start - i - 1;
        char c = origin[originIndex];
        char t = '?';
        switch (c) {
        case 'A': t = 'T'; break;
        case 'T': t = 'A'; break;
        case 'G': t = 'C'; break;
        case 'C': t = 'G'; break;
        case 'N': t = 'N'; break;
        default:
            t = 'N';
            break;
        }
        result[i] = t;
    }
    return result;
}

void GbkParser::fillIntronsAndExonsFromOrigin(SequencePtr seq)
{
    const QByteArray & origin = seq->origin;
    Q_FOREACH(GenePtr gene, seq->genes) {
        Q_FOREACH(IsoformPtr isoform, gene->isoforms) {
            fillIntronsAndExonsFromOrigin(isoform, origin);
        }
    }
}

void GbkParser::checkIsoformsMainErrors(SequencePtr seq)
{
    Q_FOREACH(GenePtr gene, seq->genes) {
        Q_FOREACH(IsoformPtr isoform, gene->isoforms) {
            checkIsoformError(isoform);
        }
    }
}

void GbkParser::checkIsoformError(IsoformPtr isoform){
    QByteArray full_exons("");

    Q_FOREACH(ExonPtr exon, isoform->exons) {
        full_exons += exon->origin;
    };
    for(int i=0; i+5<full_exons.size(); i+=3){
        if(full_exons[i]=='T'){
            if(full_exons[i+1]=='A'){
                if((full_exons[i+2]=='A')||(full_exons[i+2]=='G')){
                    // qDebug() << "----------------";
                    // qDebug() << full_exons;
                    // qDebug() << "----------------";
                    isoform->errorMain = true;
                }
            };
            if ((full_exons[i+1]=='G')&&(full_exons[i+2]=='A')){
                // qDebug() << "----------------";
                // qDebug() << full_exons;
                // qDebug() << "----------------";
                isoform->errorMain = true;
            }  
        }
    }
}

bool exonLessThan(const ExonPtr v1, const ExonPtr v2)
{
   if (v1->start < v2->start) return true;
   if (v1->start > v2->start) return false;
   if (v1->end < v2->end) return true;
   if (v1->end > v2->end) return false;
   return true;
}

void GbkParser::makeRealExons(SequencePtr seq)
{
    Q_FOREACH(GenePtr gene, seq->genes) {
        QList<ExonPtr> exons;
        QList<RealExonPtr> real_exons;
        Q_FOREACH(IsoformPtr isoform, gene->isoforms) {
            Q_FOREACH(ExonPtr exon, isoform->exons) {
                exons.push_back(exon);
            };
        }
        qSort(exons.begin(), exons.end(), exonLessThan);
        int current_id = 0;
        int current_start = -100;
        int current_end = -100;
        
        Q_FOREACH(IsoformPtr isoform, gene->isoforms) {
            Q_FOREACH(ExonPtr exon, isoform->exons) {
                if ((int(exon->start) == current_start) && (int(exon->end) == current_end)){
                    exon->real_exon_id = current_id;
                }else{
                    current_id++;
                    exon->real_exon_id = current_id;
                    current_start = exon->start;
                    current_end = exon->end;            
                }
            }
        }
    }
}

void GbkParser::fillIntronsAndExonsFromOrigin(IsoformPtr isoform,
                                              const QByteArray &origin)
{
    // qDebug() << "start parse iso:";
    // qDebug() << "origin";
    // qDebug() << origin.length();
    qint32 start = qMin(isoform->cdsStart, isoform->mrnaStart);
    qint32 end = qMax(isoform->cdsEnd, isoform->mrnaEnd);
        
    // qDebug() << start << " : " << end;
    if(end > origin.length()){
        qWarning() << "Isoform out of sequence";
        qWarning() << "File: " << isoform->sequence.toStrongRef()->sourceFileName;
        qWarning() << "Prot: " << isoform->proteinXref;
    }
    // qDebug() << "==========ORIGIN============";
    // qDebug() << origin;
    // qDebug() << "==========/ORIGIN============";
    isoform->startCodon = origin.mid(start, 3);
    isoform->endCodon = origin.mid(end-4, 3);
    // qDebug() << isoform->gene.toStrongRef()->name;
    // qDebug() << "Start: " << origin.mid(start, 3);
    // qDebug() << "End  : " << origin.mid(end-4, 3);
    bool bw = isoform->gene.toStrongRef()->backwardChain;

    const QByteArray isoformOrigin = bw
            ? dnaReverseComplement(origin, start, end)
            : origin.mid(start-1, end-start+1);

    isoform->startCodon = isoformOrigin.left(3);
    isoform->endCodon = isoformOrigin.right(3);
    // qDebug() << "==========ISOFORM ORIGIN============";
    // qDebug() << "Start: " << start << " End: " << end;
    // qDebug() << isoformOrigin;
    // qDebug() << "==========/ISOFORM ORIGIN============";

    Q_FOREACH(ExonPtr exon, isoform->exons) {
        const qint32 exonStart = exon->start;
        const qint32 exonEnd = exon->end;

        exon->origin = bw
                ? dnaReverseComplement(origin, exonStart, exonEnd)
                : origin.mid(exonStart-1, exonEnd-exonStart+1);
        // qDebug() << "EXON : ";
        // qDebug() << "Start: " << exonStart << " End: " << exonEnd;
        // qDebug() << bw;
        // qDebug() << exon->origin;
        // qDebug() << "==================";
        exon->startCodon = exon->origin.left(3);
        exon->endCodon = exon->origin.right(3);
        exon->warningNInSequence = exon->origin.contains('N');
        if (exon->warningNInSequence) {
            exon->isoform.toStrongRef()->warningInCodingExon = true;
            // exon->isoform.toStrongRef()->errorMain = true;
        }
    }

    Q_FOREACH(IntronPtr intron, isoform->introns) {
        const qint32 intronStart = intron->start;
        const qint32 intronEnd = intron->end;
        Q_ASSERT(intronStart > start);
        Q_ASSERT(intronEnd < end);

        intron->origin = bw
                ? dnaReverseComplement(origin, intronStart, intronEnd)
                : origin.mid(intronStart-1, intronEnd-intronStart+1);
        // qDebug() << "INTRON : ";
        // qDebug() << "Start: " << intronStart << " End: " << intronEnd;
        // qDebug() << bw;
        // qDebug() << intron->origin;
        // qDebug() << "==================";
        intron->startDinucleotide = intron->origin.left(2);
        intron->endDinucleotide = intron->origin.right(2);

        intron->warningInStartDinucleotide = "GT" != intron->startDinucleotide;
        intron->warningInEndDinucleotide = "AG" != intron->endDinucleotide;
        intron->errorMain =
                intron->warningInStartDinucleotide ||
                intron->warningInEndDinucleotide;
        if (intron->errorMain) {
            intron->isoform.toStrongRef()->warningInIntron = true;
            // intron->isoform.toStrongRef()->errorMain = true;
        }
        intron->warningNInSequence = intron->origin.contains('N');
    }
    // qDebug() << "finish parse iso:";
}

void GbkParser::parseRange(const QString &value,
                           quint32 *start, quint32 *end,
                           bool *bw,
                           QList<quint32> * starts, QList<quint32> * ends)
{
    *start = UINT32_MAX;
    *end = 0;
    bool complement = value.trimmed().startsWith("complement(");
    bool join =
            value.trimmed().startsWith("join(") ||
            value.trimmed().startsWith("complement(join(");
    int startPos = 0;
    int endPos = 0;
    if (!complement && !join) {
        startPos = 0;
        endPos = value.indexOf('\n');
    }
    else if ((complement && !join) || (!complement && join)) {
        startPos = value.indexOf('(') + 1;
        endPos = value.indexOf(")\n");
    }
    else if (complement && join) {
        startPos = value.indexOf("(join(") + 6;
        endPos = value.indexOf("))\n");
    }

    const QStringList rangesStrs =
            value.mid(startPos, endPos-startPos).split(QRegExp(",\\s*"));

    Q_FOREACH(const QString & rangeStr, rangesStrs) {
        QStringList words = rangeStr.split("..");
        Q_ASSERT(words.size() == 2 || words.size() == 1);
        if (1 == words.size()) {
            words.append(words[0]);
        }
        words[0].remove(QRegExp("[<>]"));
        words[1].remove(QRegExp("[<>]"));
        quint32 st = words[0].toUInt();
        quint32 en = words[1].toUInt();
        if (starts && ends) {
            starts->append(st);
            ends->append(en);
        }
        *start = qMin(*start, st);
        *end = qMax(*end, en);
    }
    *bw = complement;
}

QMap<QString, QString> GbkParser::parseFeatureAttributes(const QString &value)
{
    QRegExp rxAttr("/(\\S+)=\\\"(.+)\\\"");
    QRegExp rxDigitAttr("/(\\S+)=(\\d+)");
    QRegExp rxFlags("/(\\S+)");
    rxAttr.setMinimal(true);
    QMap<QString,QString> result;
    int pos = 0;
    Q_FOREVER {
        pos = rxAttr.indexIn(value, pos);
        if (-1 == pos) {
            break;
        }
        else {
            ++pos;
        }
        const QString key = rxAttr.cap(1);
        QString value = rxAttr.cap(2);
        if(key == "db_xref"){
            value.replace('\n', " ");
            result[key] = result[key] + "\n" + value;
        }else{
            value.replace('\n', " ");
            result[key] = value.simplified();
        }
    }
    pos = 0;
    Q_FOREVER {
        pos = rxFlags.indexIn(value, pos);
        if (-1 == pos) {
            break;
        }
        else {
            ++pos;
        }
        const QString key = rxFlags.cap(1);
        if (!result.count(key)) {
            result[key] = "";
        }
    }
    pos = 0;
    Q_FOREVER {
        pos = rxDigitAttr.indexIn(value, pos);
        if (-1 == pos) {
            break;
        }
        else {
            ++pos;
        }
        const QString key = rxDigitAttr.cap(1);
        QString value = rxDigitAttr.cap(2);
        result[key] = value.simplified();
    }
    return result;
}

