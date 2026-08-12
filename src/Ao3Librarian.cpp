#include "Ao3Librarian.h"

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <memory>

namespace {

/**
 * @brief Internal stream consumer that parses AO3 metadata on the fly.
 * Uses fixed-size buffers to avoid heap fragmentation during heavy indexing.
 */
class HtmlScraper : public Print {
 public:
  Ao3LibraryMetadata& meta;
  char buffer[1024];
  size_t bufferSize = 0;
  bool inSummary = false;
  bool inTag = false;
  size_t summaryBytes = 0;
  bool isInProgress = false;
  std::string scrapedWorkId;
  std::string scrapedDate;
  bool hasUpdatedDate = false;
  char scrapedFandom[32];
  char scrapedRel1[32];
  char scrapedRel2[32];

  explicit HtmlScraper(Ao3LibraryMetadata& m) : meta(m), scrapedWorkId(""), scrapedDate(""), hasUpdatedDate(false) {
    memset(buffer, 0, sizeof(buffer));
    bufferSize = 0;
    summaryBytes = 0;
    inSummary = false;
    inTag = false;
    isInProgress = false;
    memset(scrapedFandom, 0, sizeof(scrapedFandom));
    memset(scrapedRel1, 0, sizeof(scrapedRel1));
    memset(scrapedRel2, 0, sizeof(scrapedRel2));
  }

  void extractCommaSeparatedFields(const char* anchor, char* const outputs[], int maxCount,
                                   bool isRelationship = false) {
    const char* pos = strstr(buffer, anchor);
    if (!pos) return;

    const char* scan = pos + strlen(anchor);

    // Skip to the start of the first value: HTML tags (</dt>, <dd ...>,
    // <a href="...">, </b>), whitespace, and leading punctuation/quotes
    // (":", the opening '"' in FFF's quoted string, etc).
    while (scan < buffer + bufferSize) {
      if (*scan == '<') {
        while (scan < buffer + bufferSize && *scan != '>') scan++;
        if (scan < buffer + bufferSize) scan++;
      } else if (isspace(static_cast<unsigned char>(*scan)) || *scan == ':' || *scan == '"' || *scan == '/' ||
                 *scan == '-' || *scan == '_') {
        scan++;
      } else
        break;
    }

    int filled = 0;
    while (filled < maxCount && scan < buffer + bufferSize) {
      const char* comma = strchr(scan, ',');
      const char* lt = strchr(scan, '<');
      const char* quote = isRelationship ? nullptr : strchr(scan, '"');

      // Earliest of the three terminators that is non-null
      const char* end = nullptr;
      for (const char* cand : {comma, lt, quote}) {
        if (cand && (!end || cand < end)) end = cand;
      }
      if (!end) break;

      // Native AO3: scan sits directly on <a href="...">. Step into its
      // text content, then re-evaluate — </a> becomes the terminator.
      if (lt && lt == scan) {
        while (scan < buffer + bufferSize && *scan != '>') scan++;
        if (scan < buffer + bufferSize) scan++;
        lt = strchr(scan, '<');
        if (!lt) break;
        end = lt;
      }

      size_t rawLen = (size_t)(end - scan);
      if (rawLen == 0) {
        // Empty token (consecutive delimiters) — skip and retry
        scan = end + 1;
        continue;
      }

      if (isRelationship) {
        // If the number of quotes in the extracted token is odd,
        // the final quote is an unbalanced wrapping quote from FFF.
        size_t qCount = 0;
        for (size_t i = 0; i < rawLen; i++) {
          if (scan[i] == '"') qCount++;
        }
        if (qCount % 2 != 0 && rawLen > 0 && scan[rawLen - 1] == '"') {
          rawLen--;
        }
      }

      // &amp; cleanup
      size_t outIdx = 0;
      size_t srcIdx = 0;
      while (srcIdx < rawLen && outIdx < 31) {
        if (scan[srcIdx] == '&' && (rawLen - srcIdx) >= 5 && strncasecmp(&scan[srcIdx], "&amp;", 5) == 0) {
          outputs[filled][outIdx++] = '&';
          srcIdx += 5;
        } else {
          outputs[filled][outIdx++] = scan[srcIdx++];
        }
      }
      outputs[filled][outIdx] = '\0';
      size_t len = outIdx;

      // Trim trailing whitespace
      for (int i = (int)len - 1; i >= 0 && isspace(static_cast<unsigned char>(outputs[filled][i])); i--)
        outputs[filled][i] = '\0';
      // tag spillover prevention
      if (strcmp(outputs[filled], "Status:") == 0 || strcmp(outputs[filled], "Characters:") == 0) {
        outputs[filled][0] = '\0';  // Clear out the bogus metadata label
        break;                      // Stop parsing further slots
      }
      filled++;

      if (end == lt) {
        // Native AO3: skip past </a>
        scan = lt;
        while (scan < buffer + bufferSize && *scan != '>') scan++;
        if (scan < buffer + bufferSize) scan++;
      } else {
        // FFF / plain text: skip past the comma or closing quote
        scan = end + 1;
      }
      // Skip the separator: whitespace, ',', and stray '"' characters
      // (covers native AO3's ", " between anchors and FFF's ", ")
      while (scan < buffer + bufferSize && (isspace(static_cast<unsigned char>(*scan)) || *scan == ',' || *scan == '"'))
        scan++;
    }
  }

  size_t write(uint8_t b) override {
    char c = (char)b;

    // Append to the rolling buffer FIRST so the closing-tag check below (which
    // inspects the tail of `buffer`) can actually see the character currently
    // being processed. Checking beforehand meant the closing '>' of "</p>"/
    // "</div>"/"</span>" was never yet in `buffer` at check time, so the
    // real-time summary-close could never actually match.
    if (bufferSize < sizeof(buffer) - 1) {
      buffer[bufferSize++] = (char)c;
      buffer[bufferSize] = 0;
    }

    if (inSummary) {
      if (c == '<') inTag = true;
      if (!inTag && summaryBytes < 511) {
        meta.summary[summaryBytes++] = c;
      }
      if (c == '>') {
        inTag = false;
        // Deliberately NOT matching "</p>" here: summaries are often multiple
        // paragraphs, and every one-shot extraction path above already avoids
        // stopping on an inner </p> for that reason. This real-time carryover
        // check has to follow the same rule, or it closes prematurely the
        // instant carryover kicks in partway through a multi-paragraph summary.
        // 13 chars covers the longest closer we look for ("</blockquote>").
        if (bufferSize > 13 &&
            (strstr(buffer + bufferSize - 13, "</div>") || strstr(buffer + bufferSize - 13, "</span>") ||
             strstr(buffer + bufferSize - 13, "</blockquote>"))) {
          if (summaryBytes > 30) inSummary = false;
        }
      }
    }

    if (bufferSize > 800) {
      processBuffer();
      int overlap = bufferSize - 400;
      memmove(buffer, buffer + 400, overlap);
      bufferSize = overlap;
      buffer[bufferSize] = 0;
      yield();
    }
    return 1;
  }

  std::string extractDate(const char* anchor) {
    const char* pos = strstr(buffer, anchor);
    if (!pos) return "";

    const char* scan = pos + strlen(anchor);
    bool inHtm = false;
    while (scan < buffer + bufferSize) {
      if (*scan == '<') {
        inHtm = true;
        scan++;
        continue;
      }
      if (*scan == '>') {
        inHtm = false;
        scan++;
        continue;
      }
      if (inHtm) {
        scan++;
        continue;
      }
      if (isdigit(static_cast<unsigned char>(*scan))) {
        if (scan + 10 <= buffer + bufferSize) {
          bool valid = true;
          for (int i = 0; i < 10; i++) {
            if (i == 4 || i == 7) {
              if (scan[i] != '-') {
                valid = false;
                break;
              }
            } else {
              if (!isdigit(static_cast<unsigned char>(scan[i]))) {
                valid = false;
                break;
              }
            }
          }
          if (valid) {
            return std::string(scan, 10);
          }
        }
      }
      scan++;
    }
    return "";
  }

  void processBuffer() {
    if (meta.rating == '-') {
      findFuzzyField("Rating:", [this](const char* val) { meta.rating = Ao3Librarian::mapRating(val); });
    }

    if (meta.warning == 0 || meta.warning == '-') {
      findFuzzyField("Warnings:", [this](const char* val) {
        char w = Ao3Librarian::mapWarning(val);
        if (w != '-' || meta.warning == 0) meta.warning = w;
      });
      findFuzzyField("Archive Warning:", [this](const char* val) {
        char w = Ao3Librarian::mapWarning(val);
        if (w != '-' || meta.warning == 0) meta.warning = w;
      });
    }

    findFuzzyField("Words:", [this](const char* val) {
      char clean[32] = {0};
      int j = 0;
      for (int i = 0; val[i] && j < 31; i++)
        if (isdigit(val[i]))
          clean[j++] = val[i];
        else if (val[i] != ',')
          break;
      meta.wordCount = strtoul(clean, nullptr, 10);
    });

    // FFF-style log pages (e.g. FanFicFare downloads) label this "Word Count:"
    // instead of AO3's "Words:" — "Words:" is not a substring of "Word Count:",
    // so without this fallback these epubs would never get a word count at all,
    // and parseTitlePage()'s meta.wordCount > 0 success check would never pass.
    if (meta.wordCount == 0) {
      findFuzzyField("Word Count:", [this](const char* val) {
        char clean[32] = {0};
        int j = 0;
        for (int i = 0; val[i] && j < 31; i++)
          if (isdigit(val[i]))
            clean[j++] = val[i];
          else if (val[i] != ',')
            break;
        meta.wordCount = strtoul(clean, nullptr, 10);
      });
    }

    if (strstr(buffer, "In-Progress")) isInProgress = true;

    findFuzzyField("Chapters:", [this](const char* val) {
      const char* slash = strchr(val, '/');
      if (slash) {
        unsigned published = 0;
        for (const char* p = val; p < slash && isdigit(static_cast<unsigned char>(*p)); ++p) {
          published = published * 10u + static_cast<unsigned>(*p - '0');
          if (published > 65535u) break;
        }
        meta.chapterCount = static_cast<uint16_t>(published);
        const char* after = slash + 1;
        bool okTotal = (*after != '\0');
        for (const char* p = after; okTotal && *p; ++p) {
          if (!isdigit(static_cast<unsigned char>(*p))) okTotal = false;
        }
        unsigned total = 0;
        if (okTotal) total = static_cast<unsigned>(atoi(after));
        meta.isCompleted = okTotal && total > 0 && meta.chapterCount == static_cast<uint16_t>(total);
        // "N/?" (open-ended, no total set yet) parses as !okTotal — leave
        // totalChapters at its default 0 (unknown) in that case.
        meta.totalChapters = okTotal ? static_cast<uint16_t>(total) : 0;
      } else {
        meta.chapterCount = static_cast<uint16_t>(atoi(val));
        meta.isCompleted = !isInProgress;
        // No denominator at all in this format — if the work isn't flagged
        // in-progress, this single number IS the total (e.g. a one-shot).
        meta.totalChapters = meta.isCompleted ? meta.chapterCount : 0;
      }
    });

    // FFF title/log pages (e.g. FanFicFare downloads) don't use "Genre:" or
    // "Additional Tags:" at all — they label the same plain comma-separated
    // list just "Tags:". Compute this BEFORE extracting, and only use it as
    // a fallback when neither native label is present anywhere in the buffer:
    // "Tags:" is a substring of "Additional Tags:", so gating on tags[3]
    // alone would re-trigger on a native AO3 page that only had 1-3
    // additional tags, re-scanning from a shifted anchor position and
    // pulling a garbled 4th tag.
    bool hasNativeTagLabel = strstr(buffer, "Genre:") || strstr(buffer, "Additional Tags:");
    if (strstr(buffer, "Genre:")) extractTagsFromAnchor("Genre:");
    if (!meta.tags[3][0] && strstr(buffer, "Additional Tags:")) {
      extractTagsFromAnchor("Additional Tags:");
    }
    if (!hasNativeTagLabel && !meta.tags[3][0] && strstr(buffer, "Tags:")) {
      extractTagsFromAnchor("Tags:");
    }
    if (strstr(buffer, "Series:")) extractSeries();

    // Fandom: native AO3 uses "Fandom:" (singular, one fandom) or "Fandoms:"
    // (plural, multiple fandoms). FFF uses "Category:".
    // IMPORTANT: native AO3 epubs also contain a "Category:" field (M/M, F/F,
    // Gen, etc.) that appears *before* the fandom label in the HTML. Always try
    // both AO3 fandom labels first — falling through to "Category:" is correct
    // only for FFF epubs, which have no "Fandom:"/"Fandoms:" label at all.
    bool foundNativeFandom = false;
    char* fandomTarget[1] = {scrapedFandom};

    if (strstr(buffer, "Fandoms:")) {
      extractCommaSeparatedFields("Fandoms:", fandomTarget, 1);
      foundNativeFandom = true;
    } else if (strstr(buffer, "Fandom:")) {
      extractCommaSeparatedFields("Fandom:", fandomTarget, 1);
      foundNativeFandom = true;
    }

    // Only look for Category if no native fandom tag was found in this specific chunk,
    // AND the destination string is completely empty.
    if (!foundNativeFandom && scrapedFandom[0] == '\0') {
      if (strstr(buffer, "Category:")) {
        extractCommaSeparatedFields("Category:", fandomTarget, 1);
      }
    }

    // Relationships: native AO3 uses "Relationships:" (plural) or "Relationship:" (singular).
    // FFF-style title/log pages (e.g. FanFicFare downloads) instead split this into a
    // "Main Relationship:" label plus a separate "Additional Relationships:" label.
    // "Main Relationship:" must be checked FIRST: otherwise "Relationships:" matches as a
    // substring inside "Additional Relationships:", so the primary ship is skipped entirely
    // and the second slot fills with garbage scraped from the "Additional Relationships:" tag itself.
    // Take up to two comma-separated values. Pass true to enable custom nickname processing.
    if (scrapedRel1[0] == '\0' && scrapedRel2[0] == '\0') {
      const char* relAnchor = nullptr;
      bool splitMainAdditional = false;
      if (strstr(buffer, "Main Relationship:")) {
        relAnchor = "Main Relationship:";
        splitMainAdditional = true;
      } else if (strstr(buffer, "Relationships:")) {
        relAnchor = "Relationships:";
      } else if (strstr(buffer, "Relationship:")) {
        relAnchor = "Relationship:";
      }

      if (relAnchor) {
        char* targets[2] = {scrapedRel1, scrapedRel2};
        extractCommaSeparatedFields(relAnchor, targets, splitMainAdditional ? 1 : 2, true);

        // Pull the second ship from its own separate label rather than letting
        // extractCommaSeparatedFields wander past </dd> into the next <dt>/<b> tags.
        if (splitMainAdditional && scrapedRel2[0] == '\0' && strstr(buffer, "Additional Relationships:")) {
          char* extra[1] = {scrapedRel2};
          extractCommaSeparatedFields("Additional Relationships:", extra, 1, true);
        }

        for (char* rel : targets) {
          if (rel[0] == '\0') continue;

          char cleanBuf[32];
          size_t cIdx = 0;
          bool inParen = false;

          // Look at the raw string we grabbed, but process it intelligently
          for (size_t i = 0; rel[i] != '\0'; i++) {
            if (rel[i] == '(') {
              inParen = true;
              // Backtrack to remove a single space before the parenthesis if present
              if (cIdx > 0 && cleanBuf[cIdx - 1] == ' ') {
                cIdx--;
              }
            } else if (rel[i] == ')') {
              inParen = false;
            } else if (!inParen) {
              // The 31-character limit is evaluated here,
              // after skipping everything inside parentheses!
              if (cIdx < 31) {
                // Prevent duplicate spaces caused by stripping from the middle
                if (rel[i] == ' ' && cIdx > 0 && cleanBuf[cIdx - 1] == ' ') {
                  continue;
                }
                cleanBuf[cIdx++] = rel[i];
              }
            }
          }
          cleanBuf[cIdx] = '\0';

          // Trim any leftover trailing whitespace safely
          while (cIdx > 0 && isspace(static_cast<unsigned char>(cleanBuf[cIdx - 1]))) {
            cleanBuf[--cIdx] = '\0';
          }

          // Copy the isolated, clean string back into the persistent variable
          strcpy(rel, cleanBuf);
        }
      }
    }

    const char* sumPos = strstr(buffer, "Summary:");
    if (sumPos && !inSummary && meta.summary[0] == '\0') {
      const char* scan = sumPos + (sizeof("Summary:") - 1);
      bool htm = false;
      while (scan < buffer + bufferSize) {
        if (*scan == '<')
          htm = true;
        else if (*scan == '>')
          htm = false;
        else if (!htm && !isspace(static_cast<unsigned char>(*scan)) && *scan != ':' && *scan != '-' && *scan != '/') {
          inSummary = true;
          while (scan < buffer + bufferSize && summaryBytes < 511) {
            if (*scan == '<') {
              htm = true;
            } else if (*scan == '>') {
              htm = false;
              // Stop as soon as we hit the tag that closes the summary's own
              // outer wrapper (</div> or </span>) instead of greedily consuming
              // the rest of the buffer — otherwise whatever field follows on
              // the same line (e.g. FFF log pages put "Tags:" right after
              // "...belonged all along.</p></div></span>") gets swallowed
              // into the summary text too. Deliberately NOT stopping on </p>:
              // summaries are often multiple paragraphs, and stopping at the
              // first one would truncate the rest.
              if (summaryBytes > 30) {
                const char* tagStart = scan;
                while (tagStart > buffer && *tagStart != '<') tagStart--;
                size_t tagLen = (size_t)(scan - tagStart) + 1;
                if (tagLen <= 8) {
                  char tagBuf[9] = {0};
                  memcpy(tagBuf, tagStart, tagLen);
                  if (strcmp(tagBuf, "</div>") == 0 || strcmp(tagBuf, "</span>") == 0) {
                    scan++;
                    inSummary = false;
                    break;
                  }
                }
              }
            } else if (!htm) {
              meta.summary[summaryBytes++] = *scan;
            }
            scan++;
          }
          break;
        }
        scan++;
      }
    }

    if (meta.summary[0] == '\0') tryExtractNativeSummary();
    if (meta.summary[0] == '\0') tryExtractFFFTitlePageSummary();

    // Native AO3: extract work ID from works/ URL in HTML
    const char* pos = strstr(buffer, "archiveofourown.org/works/");
    if (pos) {
      const char* p = pos + 26;
      std::string extractedId = "";
      while (*p && isdigit(static_cast<unsigned char>(*p))) {
        extractedId += *p;
        p++;
      }
      if (extractedId.size() > scrapedWorkId.size()) {
        scrapedWorkId = extractedId;
      }
    }

    // Extract last updated or published date with Updated having absolute precedence
    std::string tempDate = extractDate("Updated:");
    if (!tempDate.empty()) {
      scrapedDate = tempDate;
      hasUpdatedDate = true;
    } else if (!hasUpdatedDate) {
      tempDate = extractDate("Published:");
      if (!tempDate.empty()) {
        scrapedDate = tempDate;
      }
    }
  }

  void tryExtractNativeSummary() {
    const char* mark = strstr(buffer, ">Summary</");
    if (!mark) mark = strstr(buffer, ">Summary<");
    if (!mark) return;
    const char* bq = strstr(mark, "<blockquote");
    if (!bq) return;
    const char* gt = strchr(bq, '>');
    if (!gt) return;
    const char* scan = gt + 1;
    // Use the shared inTag/summaryBytes members (not locals) so that if
    // </blockquote> doesn't fall within the CURRENT buffer window, write()'s
    // real-time per-byte handling can pick up exactly where this one-shot
    // pass left off on subsequent bytes — otherwise a summary spanning a
    // buffer-flush boundary gets truncated mid-sentence with no way to
    // recover the rest (as happened on a 21-chapter native AO3 epub whose
    // blockquote summary ran past the first 800-byte window).
    inTag = false;
    summaryBytes = strlen(meta.summary);
    bool closed = false;
    while (scan < buffer + bufferSize && summaryBytes < 511) {
      if (*scan == '<') {
        if (static_cast<size_t>(buffer + bufferSize - scan) >= 13 && strncmp(scan, "</blockquote>", 13) == 0) {
          closed = true;
          break;
        }
        inTag = true;
      } else if (*scan == '>') {
        inTag = false;
      } else if (!inTag) {
        meta.summary[summaryBytes++] = *scan;
      }
      scan++;
    }
    meta.summary[summaryBytes] = '\0';
    if (!closed && summaryBytes < 511) {
      // Ran out of buffer before hitting </blockquote> — hand off to write()'s
      // real-time closing check for the rest of the stream.
      inSummary = true;
    }
  }

  // Some FFF title pages (no separate log page — e.g. "They All Taste the
  // Same") wrap the summary as <div class="tag-summary-content"><div
  // class="userstuff">...text...</div></div>, with no "Summary:" label and
  // no <blockquote> at all, so neither of the other two paths ever fires.
  // The first </div> reached after entering the wrapper is the inner
  // userstuff div's own close (there's no other nested div inside AO3
  // summary text), so that's the correct, safe stop point.
  void tryExtractFFFTitlePageSummary() {
    const char* mark = strstr(buffer, "tag-summary-content");
    if (!mark) return;
    const char* gt = strchr(mark, '>');
    if (!gt) return;
    const char* scan = gt + 1;
    inTag = false;
    summaryBytes = strlen(meta.summary);
    bool closed = false;
    while (scan < buffer + bufferSize && summaryBytes < 511) {
      if (*scan == '<') {
        if (static_cast<size_t>(buffer + bufferSize - scan) >= 6 && strncmp(scan, "</div>", 6) == 0) {
          closed = true;
          break;
        }
        inTag = true;
      } else if (*scan == '>') {
        inTag = false;
      } else if (!inTag) {
        meta.summary[summaryBytes++] = *scan;
      }
      scan++;
    }
    meta.summary[summaryBytes] = '\0';
    if (!closed && summaryBytes < 511) {
      // Ran out of buffer before hitting </div> — hand off to write()'s
      // real-time closing check (already recognizes </div>) for the rest.
      inSummary = true;
    }
  }

  void findFuzzyField(const char* anchor, std::function<void(const char*)> callback) {
    const char* pos = strstr(buffer, anchor);
    if (!pos) return;

    const char* scan = pos + strlen(anchor);
    bool inHtm = false;
    const char* valStart = nullptr;

    while (scan < buffer + bufferSize) {
      if (*scan == '<') {
        inHtm = true;
        scan++;
        continue;
      }
      if (*scan == '>') {
        inHtm = false;
        scan++;
        continue;
      }
      if (!inHtm && !isspace(*scan) && *scan != ':' && *scan != '-' && *scan != '/' && *scan != 's' && *scan != 'S') {
        valStart = scan;
        break;
      }
      scan++;
    }

    if (!valStart) return;

    const char* valEnd = strchr(valStart, '<');
    if (valEnd) {
      char temp[128];
      size_t len = std::min((size_t)(valEnd - valStart), sizeof(temp) - 1);
      strncpy(temp, valStart, len);
      temp[len] = 0;
      while (len > 0 && isspace(temp[len - 1])) temp[--len] = 0;
      callback(temp);
    }
  }

  void extractSeries() {
    const char* pos = strstr(buffer, "Series:");
    if (!pos) return;

    const char* scan = pos + (sizeof("Series:") - 1);
    // Skip to the start of the value
    while (scan < buffer + bufferSize) {
      if (*scan == '<') {
        while (scan < buffer + bufferSize && *scan != '>') scan++;
        if (scan < buffer + bufferSize) scan++;
      } else if (isspace(*scan) || *scan == ':' || *scan == '/' || *scan == '-' || *scan == '_') {
        scan++;
      } else {
        break;
      }
    }

    if (scan >= buffer + bufferSize) return;

    // Read characters, ignoring HTML tags, until we hit a newline or EOF.
    char seriesText[256];
    size_t outIdx = 0;
    while (scan < buffer + bufferSize && *scan != '\n' && *scan != '\r' && outIdx < sizeof(seriesText) - 1) {
      if (*scan == '<') {
        while (scan < buffer + bufferSize && *scan != '>') scan++;
      } else if (*scan != '>') {
        seriesText[outIdx++] = *scan;
      }
      scan++;
    }
    seriesText[outIdx] = 0;

    // Clean up trailing spaces
    while (outIdx > 0 && isspace(seriesText[outIdx - 1])) {
      seriesText[--outIdx] = 0;
    }

    // Native AO3: "part N of Series Title"
    {
      char* p = seriesText;
      while (*p && isspace(static_cast<unsigned char>(*p))) p++;
      if (strncasecmp(p, "part ", 5) == 0) {
        const char* num = p + 5;
        int part = atoi(num);
        while (*num && isdigit(static_cast<unsigned char>(*num))) num++;
        while (*num && isspace(static_cast<unsigned char>(*num))) num++;
        if (strncasecmp(num, "of ", 3) == 0) {
          num += 3;
          while (*num && isspace(static_cast<unsigned char>(*num))) num++;
          if (part > 0 && *num) {
            meta.seriesPart = static_cast<uint16_t>(part);
            strncpy(meta.seriesName, num, sizeof(meta.seriesName) - 1);
            meta.seriesName[sizeof(meta.seriesName) - 1] = '\0';
            return;
          }
        }
      }
    }

    // FanFicFare: "Title [N]"
    char* bracketPos = strrchr(seriesText, '[');
    if (bracketPos) {
      int part = atoi(bracketPos + 1);
      if (part > 0) meta.seriesPart = part;

      *bracketPos = 0;  // Terminate name before bracket
      size_t nameLen = strlen(seriesText);
      while (nameLen > 0 && isspace(seriesText[nameLen - 1])) {
        seriesText[--nameLen] = 0;
      }
      strncpy(meta.seriesName, seriesText, sizeof(meta.seriesName) - 1);
      meta.seriesName[sizeof(meta.seriesName) - 1] = '\0';
    } else {
      strncpy(meta.seriesName, seriesText, sizeof(meta.seriesName) - 1);
      meta.seriesName[sizeof(meta.seriesName) - 1] = '\0';
    }
  }

  void extractTagsFromAnchor(const char* anchor) {
    const char* pos = strstr(buffer, anchor);
    if (!pos) return;

    const char* scan = pos + strlen(anchor);
    // Properly skip whole HTML tags and delimiters, so </b> doesn't leave 'b>' behind
    while (scan < buffer + bufferSize) {
      if (*scan == '<') {
        while (scan < buffer + bufferSize && *scan != '>') scan++;
        if (scan < buffer + bufferSize) scan++;  // skip '>'
      } else if (isspace(*scan) || *scan == ':' || *scan == '/' || *scan == '-' || *scan == '_') {
        scan++;
      } else {
        break;
      }
    }
    // Skip past already-extracted tags in the HTML source so the scan
    // position stays in sync with the destination slot index. Mirrors the
    // same comma-or-'<' boundary detection the real extraction loop below
    // uses, rather than assuming </a>-linked tags (native AO3's format) —
    // FFF's plain comma-separated "Tags:" list has no <a> at all, so an
    // </a>-only skip can never advance past it on a later buffer flush,
    // and ends up re-reading the same early tags into later slots.
    int alreadyFilled = 0;
    while (alreadyFilled < 4 && meta.tags[alreadyFilled][0]) alreadyFilled++;

    for (int i = 0; i < alreadyFilled && scan < buffer + bufferSize; i++) {
      const char* skipComma = strchr(scan, ',');
      const char* skipLt = strchr(scan, '<');
      const char* skipEnd = (skipComma && skipLt) ? std::min(skipComma, skipLt) : (skipComma ? skipComma : skipLt);
      if (!skipEnd) break;

      if (skipLt && skipLt == scan) {
        // Native AO3: this entry is an <a href="...">tag</a> link.
        while (scan < buffer + bufferSize && *scan != '>') scan++;
        if (scan >= buffer + bufferSize) break;
        scan++;
        skipLt = strchr(scan, '<');
        if (!skipLt) break;
        skipEnd = skipLt;
      }

      if (skipEnd == skipLt) {
        scan = skipLt;
        while (scan < buffer + bufferSize && *scan != '>') scan++;
        if (scan < buffer + bufferSize) scan++;
        while (scan < buffer + bufferSize && (isspace(*scan) || *scan == ',' || *scan == '"')) scan++;
      } else {
        // FFF: plain comma-separated text — just advance past the comma.
        scan = skipEnd + 1;
        while (scan < buffer + bufferSize && (isspace(*scan) || *scan == ',')) scan++;
      }
    }

    // check tagIdx and find the first empty slot
    int tagIdx = alreadyFilled;

    while (tagIdx < 4 && scan < buffer + bufferSize) {
      const char* comma = strchr(scan, ',');
      const char* lt = strchr(scan, '<');
      const char* actualEnd = (comma && lt) ? std::min(comma, lt) : (comma ? comma : lt);
      if (!actualEnd) break;

      if (lt && lt == scan) {
        // Native AO3: scan is sitting on an <a href="...">, skip into its text content
        while (scan < buffer + bufferSize && *scan != '>') scan++;

        // boundary check after pointer advancement
        if (scan >= buffer + bufferSize) break;
        scan++;  // skip '>'

        // Recalculate lt and actualEnd now that we're inside the tag text
        lt = strchr(scan, '<');
        if (!lt) break;
        actualEnd = lt;
      }

      size_t rawLen = (size_t)(actualEnd - scan);
      if (rawLen == 0) {
        scan = lt;
        continue;
      }  // empty text node, skip

      // Decode HTML entities while copying into dst.
      // Currently handles &amp; → &. Returns decoded byte count.
      auto decodeEntities = [](char* dst, size_t dstMax, const char* src, size_t srcLen) -> size_t {
        size_t out = 0, in = 0;
        while (in < srcLen && out < dstMax) {
          if (src[in] == '&' && srcLen - in >= 5 && strncasecmp(src + in, "&amp;", 5) == 0) {
            dst[out++] = '&';
            in += 5;
          } else {
            dst[out++] = src[in++];
          }
        }
        dst[out] = '\0';
        return out;
      };

      char tempTag[64] = {0};
      size_t processLen = 0;

      const char* auPrefix = "Alternate Universe - ";
      const size_t auLen = 21;

      const char* irPrefix = "Implied/Referenced ";
      const size_t irLen = 19;

      if (rawLen > auLen && strncasecmp(scan, auPrefix, auLen) == 0) {
        strcpy(tempTag, "AU-");
        size_t decoded = decodeEntities(tempTag + 3, sizeof(tempTag) - 4, scan + auLen, rawLen - auLen);
        processLen = 3 + decoded;

      } else if (rawLen > irLen && strncasecmp(scan, irPrefix, irLen) == 0) {
        strcpy(tempTag, "I/R ");
        size_t decoded = decodeEntities(tempTag + 4, sizeof(tempTag) - 5, scan + irLen, rawLen - irLen);
        processLen = 4 + decoded;

      } else {
        // Standard tag: decode entities before measuring/truncating
        processLen = decodeEntities(tempTag, sizeof(tempTag) - 1, scan, rawLen);
      }

      bool truncated = processLen > 15;
      size_t len = std::min(processLen, truncated ? (size_t)14 : (size_t)15);
      strncpy(meta.tags[tagIdx], tempTag, len);
      meta.tags[tagIdx][len] = 0;

      // trim space
      size_t cleanLen = len;
      while (cleanLen > 0 && isspace(static_cast<unsigned char>(meta.tags[tagIdx][cleanLen - 1]))) {
        meta.tags[tagIdx][--cleanLen] = '\0';
      }

      // remove Other tag and leave others blank
      if (tagIdx == 0 && (strcmp(meta.tags[tagIdx], "Other") == 0 || strncmp(meta.tags[tagIdx], "Other", 5) == 0)) {
        meta.tags[tagIdx][0] = '\0';
        break;
      }

      if (truncated) {
        // If cleanLen was reduced by trimming a space, we have room for 2 dots to reach 15 chars
        if (cleanLen < 14) {
          meta.tags[tagIdx][cleanLen] = '.';
          meta.tags[tagIdx][cleanLen + 1] = '.';
          meta.tags[tagIdx][cleanLen + 2] = '\0';
        } else {
          // Normal mid-word truncation (1 dot)
          meta.tags[tagIdx][cleanLen] = '.';
          meta.tags[tagIdx][cleanLen + 1] = '\0';
        }
      }

      tagIdx++;

      if (actualEnd == lt) {
        // Native AO3: skip past </a> closing tag and the ", " separator (including quotes)
        scan = lt;
        while (scan < buffer + bufferSize && *scan != '>') scan++;
        if (scan < buffer + bufferSize) scan++;  // skip '>'
        while (scan < buffer + bufferSize && (isspace(*scan) || *scan == ',' || *scan == '"')) scan++;
      } else {
        // FFF: plain comma-separated text, just advance past the comma and whitespace
        scan = actualEnd + 1;
        while (scan < buffer + bufferSize && (isspace(*scan) || *scan == ',')) scan++;
      }
    }
  }
};

/**
 * @brief Minimal streaming word counter: strips HTML tags and counts
 * whitespace-delimited tokens. Deliberately simple — this only needs to
 * produce a reasonable estimate, not an exact count, and has to run over
 * every chapter of the book without meaningfully slowing down import.
 */
class WordCounter : public Print {
 public:
  uint32_t words = 0;

  size_t write(uint8_t b) override {
    char c = (char)b;
    if (c == '<') {
      inTag = true;
      inWord = false;  // a tag boundary always ends the current word
      return 1;
    }
    if (c == '>') {
      inTag = false;
      return 1;
    }
    if (inTag) return 1;

    if (!isspace(static_cast<unsigned char>(c))) {
      if (!inWord) {
        words++;
        inWord = true;
      }
    } else {
      inWord = false;
    }
    return 1;
  }

 private:
  bool inTag = false;
  bool inWord = false;
};

}  // namespace

uint32_t Ao3Librarian::estimateWordCount(const Epub& epub) {
  const int spineCount = epub.getSpineItemsCount();
  if (spineCount == 0) return 0;

  // Skip known non-chapter spine items by filename rather than position:
  // native AO3 (calibre) epubs put everything in one numbered "split_NNN"
  // sequence with no distinct cover entry, while FFF epubs use clearly
  // named cover/title_page/log_page items — position alone can't tell
  // them apart, but the filenames reliably can for the formats we scrape.
  static const char* skipMarkers[] = {"cover",   "title_page", "titlepage", "log_page",
                                      "logpage", "toc",        "nav",       "copyright"};

  uint32_t total = 0;
  for (int i = 0; i < spineCount; i++) {
    std::string href = epub.getSpineItem(i).href;
    std::string lowerHref = href;
    for (char& c : lowerHref) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

    bool skip = false;
    for (const char* marker : skipMarkers) {
      if (lowerHref.find(marker) != std::string::npos) {
        skip = true;
        break;
      }
    }
    if (skip) continue;

    WordCounter counter;
    if (epub.readItemContentsToStream(href, counter, 8192)) {
      total += counter.words;
    }
    yield();
  }
  return total;
}

bool Ao3Librarian::scrape(const Epub& epub, bool force) {
  const std::string infoPath = epub.getCachePath() + "/ao3_library_info";

  if (!force && Storage.exists(infoPath.c_str())) {
    auto existing = std::unique_ptr<Ao3LibraryMetadata>(new Ao3LibraryMetadata());
    if (getLibraryInfo(epub, *existing)) {
      if (existing->version == 9 && existing->rating != '-' && existing->warning != 0 && existing->summary[0] != 0 &&
          existing->wordCount > 0) {
        // If ao3-info.bin is missing but we already have cached library info,
        // we can still attempt to rebuild ao3-info.bin if we sniff a native preface
        if (!epub.hasAo3Info() && sniffNativeAo3Preface(epub)) {
          auto meta = std::unique_ptr<Ao3LibraryMetadata>(new Ao3LibraryMetadata());
          std::string scrapedWorkId = "";
          std::string scrapedDate = "";
          char tempFandom[32] = {};
          char tempRel1[32] = {};
          char tempRel2[32] = {};
          if (parseTitlePage(epub, *meta, scrapedWorkId, scrapedDate, tempFandom, tempRel1, tempRel2)) {
            if (!scrapedWorkId.empty()) {
              epub.saveAo3Info(scrapedWorkId, scrapedDate, meta->isCompleted);
            }
          }
        }
        return true;
      }
    }
  }

  auto meta = std::unique_ptr<Ao3LibraryMetadata>(new Ao3LibraryMetadata());
  std::string scrapedWorkId = "";
  std::string scrapedDate = "";
  char scrapedFandom[32] = {};
  char scrapedRel1[32] = {};
  char scrapedRel2[32] = {};

  if (!parseTitlePage(epub, *meta, scrapedWorkId, scrapedDate, scrapedFandom, scrapedRel1, scrapedRel2)) return false;

  strncpy(meta->filepath, epub.getPath().c_str(), 255);

  // Clean up "&amp;" in the title
  std::string title = epub.getTitle();
  size_t titlePos = 0;
  while ((titlePos = title.find("&amp;", titlePos)) != std::string::npos) {
    title.replace(titlePos, 5, "&");
    titlePos += 1;  // Advance past the newly inserted '&'
  }
  strncpy(meta->title, title.c_str(), 127);
  meta->title[127] = '\0';

  // Clean up "&amp;" in the author name (covers co-authored fics!)
  std::string author = epub.getAuthor();
  size_t authorPos = 0;
  while ((authorPos = author.find("&amp;", authorPos)) != std::string::npos) {
    author.replace(authorPos, 5, "&");
    authorPos += 1;
  }
  strncpy(meta->author, author.c_str(), 127);
  meta->author[127] = '\0';

  strncpy(meta->updatedDate, scrapedDate.c_str(), sizeof(meta->updatedDate) - 1);
  meta->updatedDate[sizeof(meta->updatedDate) - 1] = '\0';

  // If nothing we scraped exposed a real word count (e.g. an FFF title page
  // with no separate log page), estimate one by counting words across the
  // actual chapter content. This walks every chapter via the zip reader, so
  // it costs noticeably more time/I-O than a normal scrape — but it only
  // runs once: the estimate gets cached in the sidecar like everything else,
  // and future scrapes see wordCount > 0 and skip straight past this.
  if (meta->wordCount == 0) {
    uint32_t estimated = estimateWordCount(epub);
    if (estimated > 0) {
      meta->wordCount = estimated;
      meta->wordCountIsEstimate = 1;
    }
  }

  // Generate the ao3-info.bin sidecar if missing and we extracted a valid Work ID
  if (!epub.hasAo3Info() && !scrapedWorkId.empty()) {
    epub.saveAo3Info(scrapedWorkId, scrapedDate, meta->isCompleted);
  }

  HalFile f;
  if (Storage.openFileForWrite("AO3L", infoPath, f)) {
    f.write((uint8_t*)meta.get(), sizeof(Ao3LibraryMetadata));
    f.close();

    CompactIndexRecord rec;
    memset(&rec, 0, sizeof(rec));

    strncpy(rec.title, meta->title, 63);
    strncpy(rec.author, meta->author, 31);
    strncpy(rec.seriesName, meta->seriesName, 31);
    strncpy(rec.fandom, scrapedFandom, 31);
    strncpy(rec.relationship1, scrapedRel1, 31);
    strncpy(rec.relationship2, scrapedRel2, 31);
    rec.wordCount = meta->wordCount;
    rec.seriesPart = meta->seriesPart;
    rec.rating = meta->rating;
    rec.isCompleted = meta->isCompleted;
    rec.cacheHash = static_cast<uint32_t>(std::hash<std::string>{}(epub.getPath()));

    if (!writeIndexRecord(rec)) {
      return false;
    }

    return true;
  }
  return false;
}

bool Ao3Librarian::getLibraryInfo(const Epub& epub, Ao3LibraryMetadata& meta) {
  const std::string infoPath = epub.getCachePath() + "/ao3_library_info";
  HalFile f;
  if (Storage.openFileForRead("AO3L", infoPath, f)) {
    if (f.read((uint8_t*)&meta, sizeof(meta)) == sizeof(meta)) {
      f.close();
      return meta.isValid();
    }
    f.close();
  }
  return false;
}

bool Ao3Librarian::parseTitlePage(const Epub& epub, Ao3LibraryMetadata& meta, std::string& scrapedWorkId,
                                  std::string& scrapedDate, char* scrapedFandom, char* scrapedRel1, char* scrapedRel2) {
  int spineCount = epub.getSpineItemsCount();
  if (spineCount == 0) return false;

  bool foundInfoSpine = false;

  auto scanSpineItem = [&](int i) {
    std::string href = epub.getSpineItem(i).href;
    auto scraper = std::unique_ptr<HtmlScraper>(new HtmlScraper(meta));

    if (epub.readItemContentsToStream(href, *scraper, 8192)) {
      scraper->processBuffer();

      // workId/date may be in a different spine (e.g. native AO3 preface) —
      // propagate from whichever spine finds them first, as before.
      if (scrapedWorkId.empty() && !scraper->scrapedWorkId.empty()) {
        scrapedWorkId = scraper->scrapedWorkId;
      }
      if (scrapedDate.empty() && !scraper->scrapedDate.empty()) {
        scrapedDate = scraper->scrapedDate;
      }

      // Fandom/relationship: take both from the FIRST spine where either is
      // found, as a unit — including an empty relationship as final. Some
      // FFF-style downloads have no Fandom:/Fandoms:/Category: label at all,
      // so gating this on fandom alone would silently discard relationships
      // that were correctly parsed from the very same spine.
      if (!foundInfoSpine && (scraper->scrapedFandom[0] != '\0' || scraper->scrapedRel1[0] != '\0')) {
        foundInfoSpine = true;
        strncpy(scrapedFandom, scraper->scrapedFandom, 31);
        scrapedFandom[31] = '\0';
        strncpy(scrapedRel1, scraper->scrapedRel1, 31);
        scrapedRel1[31] = '\0';
        strncpy(scrapedRel2, scraper->scrapedRel2, 31);
        scrapedRel2[31] = '\0';
      }
    }
  };

  // Check up to the first 3 spine items to account for internal cover pages.
  // Native AO3 (calibre-generated) epubs keep all metadata here.
  int itemsToCheck = std::min(3, spineCount);
  for (int i = 0; i < itemsToCheck; i++) {
    scanSpineItem(i);
  }

  // FFF-style epubs (e.g. FanFicFare downloads) instead put a fuller
  // "log page" — word count, published/updated dates, summary, full
  // relationship list — as the LAST spine item rather than the front
  // matter. If the front pages didn't yield a word count, check the
  // final spine item before giving up.
  if (meta.wordCount == 0 && spineCount > itemsToCheck) {
    scanSpineItem(spineCount - 1);
  }

  // Some FFF-style downloads (e.g. FanFicFare epubs with a title page but no
  // separate log page — see "They All Taste the Same") never expose a word
  // count anywhere in the readable HTML at all; it only exists, if at all, as
  // a Calibre custom-column value buried in content.opf, which this scraper
  // doesn't parse. Requiring wordCount > 0 would make scrape() fail outright
  // for these even though rating/chapters/relationships/tags/summary/workId
  // all parsed correctly. Treat a definitively-identified AO3 work (via its
  // canonical archiveofourown.org/works/<id> URL) as success too.
  return meta.wordCount > 0 || !scrapedWorkId.empty();
}

bool Ao3Librarian::sniffNativeAo3Preface(const Epub& epub) {
  if (epub.getSpineItemsCount() <= 0) return false;

  struct PrefaceSniffer final : public Print {
    char buf[2400];
    size_t n = 0;
    bool match = false;
    size_t write(uint8_t b) override {
      if (match) return 1;
      if (n + 1 < sizeof(buf)) {
        buf[n++] = static_cast<char>(b);
        buf[n] = '\0';
      }
      if (n >= 40 && !match) {
        if (strstr(buf, "archiveofourown.org/works/"))
          match = true;
        else if (strstr(buf, "Posted originally on") && strstr(buf, "archiveofourown"))
          match = true;
      }
      return 1;
    }
  };

  auto sniffer = std::unique_ptr<PrefaceSniffer>(new PrefaceSniffer());
  epub.readItemContentsToStream(epub.getSpineItem(0).href, *sniffer, 8192);
  return sniffer->match;
}

char Ao3Librarian::mapRating(const char* s) {
  if (strcasestr(s, "General")) return 'G';
  if (strcasestr(s, "Teen")) return 'T';
  if (strcasestr(s, "Mature")) return 'M';
  if (strcasestr(s, "Explicit")) return 'E';
  return '-';
}

char Ao3Librarian::mapWarning(const char* s) {
  if (strcasestr(s, "Creator Chose Not To Use") || strcasestr(s, "Choose Not To Use Archive Warnings")) return 'B';
  if (strcasestr(s, "No Archive Warnings Apply")) return '-';
  if (strlen(s) > 3) return '!';
  return '-';
}

void Ao3Librarian::scanGlobalLibrary(std::vector<Ao3LibraryMetadata>& out) {
  const char* cacheRoot = "/.crosspoint";
  HalFile root = Storage.open(cacheRoot);
  if (!root || !root.isDirectory()) return;

  HalFile entry;
  while (entry = root.openNextFile()) {
    char name[64];
    entry.getName(name, sizeof(name));
    if (entry.isDirectory() && strncmp(name, "epub_", 5) == 0) {
      std::string infoPath = std::string(cacheRoot) + "/" + name + "/ao3_library_info";
      if (Storage.exists(infoPath.c_str())) {
        HalFile f;
        if (Storage.openFileForRead("AO3L", infoPath, f)) {
          Ao3LibraryMetadata meta;
          if (f.read((uint8_t*)&meta, sizeof(meta)) == sizeof(meta)) {
            if (meta.isValid() && meta.version == 9) {
              out.push_back(meta);
            }
          }
          f.close();
        }
      }
    }
    entry.close();
    yield();
  }
  root.close();
}

bool Ao3Librarian::hasAnyAo3Fics() {
  const char* cacheRoot = "/.crosspoint";
  HalFile root = Storage.open(cacheRoot);
  if (!root || !root.isDirectory()) return false;

  bool found = false;
  HalFile entry;
  while (entry = root.openNextFile()) {
    char name[64];
    entry.getName(name, sizeof(name));
    if (entry.isDirectory() && strncmp(name, "epub_", 5) == 0) {
      std::string infoPath = std::string(cacheRoot) + "/" + name + "/ao3_library_info";
      if (Storage.exists(infoPath.c_str())) {
        found = true;
        entry.close();
        break;
      }
    }
    entry.close();
    yield();
  }
  root.close();
  return found;
}

bool Ao3Librarian::writeIndexRecord(const CompactIndexRecord& rec) {
  const char* indexPath = "/.crosspoint/ao3_library_index.bin";

  // --- Validate existing file ---
  bool needsCreate = false;
  if (!Storage.exists(indexPath)) {
    needsCreate = true;
  } else {
    HalFile check;
    if (Storage.openFileForRead("AO3L", indexPath, check)) {
      char magic[4];
      uint8_t version;
      uint16_t recordCountCheck;
      bool readOk =
          check.read(magic, 4) == 4 && check.read(&version, 1) == 1 && check.read((uint8_t*)&recordCountCheck, 2) == 2;
      check.close();
      if (!readOk || memcmp(magic, "AO3X", 4) != 0 || version != 2 || recordCountCheck > MAX_LIBRARY_BOOKS) {
        Storage.remove(indexPath);
        needsCreate = true;
      }
    } else {
      needsCreate = true;
    }
  }

  // --- Create fresh file with empty header if needed ---
  if (needsCreate) {
    HalFile f;
    if (!Storage.openFileForWrite("AO3L", indexPath, f)) return false;
    uint8_t v = 2, r = 0;
    uint16_t c = 0;
    uint32_t s = 0;
    f.write((uint8_t*)"AO3X", 4);
    f.write(&v, 1);
    f.write((uint8_t*)&c, 2);
    f.write((uint8_t*)&s, 4);
    f.write(&r, 1);
    f.close();
  }

  // --- Open for read/write ---
  HalFile f = Storage.open(indexPath, O_RDWR);
  if (!f) return false;

  char magic[4];
  uint8_t version;
  uint16_t recordCount;
  uint32_t nextSequence;
  uint8_t reserved[1];
  f.read(magic, 4);
  f.read(&version, 1);
  f.read((uint8_t*)&recordCount, 2);
  f.read((uint8_t*)&nextSequence, 4);
  f.read(reserved, 1);

  int32_t updateSlot = -1;
  uint32_t preservedSeq = 0;
  int32_t freeSlot = -1;
  uint16_t liveCount = 0;
  CompactIndexRecord existing;
  for (uint16_t i = 0; i < recordCount; i++) {
    if (f.read((uint8_t*)&existing, sizeof(existing)) != sizeof(existing)) {
      break;
    }
    if (existing.flags & 1) {
      if (freeSlot < 0) freeSlot = i;
    } else {
      liveCount++;
      if (existing.cacheHash == rec.cacheHash) {
        updateSlot = i;
        preservedSeq = existing.addedSequence;
      }
    }
  }

  CompactIndexRecord recToWrite = rec;

  if (updateSlot >= 0) {
    recToWrite.addedSequence = preservedSeq;
    f.seek(offsetOf(updateSlot));
    f.write((uint8_t*)&recToWrite, sizeof(recToWrite));
    f.close();
    return true;
  }

  recToWrite.addedSequence = nextSequence;
  nextSequence++;

  if (freeSlot >= 0) {
    f.seek(offsetOf(freeSlot));
    f.write((uint8_t*)&recToWrite, sizeof(recToWrite));
  } else {
    if (liveCount >= MAX_LIBRARY_BOOKS) {
      f.close();
      return false;
    }
    f.seek(f.size());
    f.write((uint8_t*)&recToWrite, sizeof(recToWrite));
    recordCount++;
    f.seek(5);
    f.write((uint8_t*)&recordCount, 2);
  }

  f.seek(7);
  f.write((uint8_t*)&nextSequence, 4);

  f.close();
  return true;
}

bool Ao3Librarian::tombstoneRecord(const std::string& epubPath) {
  const char* indexPath = "/.crosspoint/ao3_library_index.bin";
  if (!Storage.exists(indexPath)) return false;

  HalFile f = Storage.open(indexPath, O_RDWR);
  if (!f) return false;

  char magic[4];
  uint8_t version;
  uint16_t recordCount;
  bool readOk = f.read(magic, 4) == 4 && f.read(&version, 1) == 1 && f.read((uint8_t*)&recordCount, 2) == 2;

  if (!readOk || memcmp(magic, "AO3X", 4) != 0 || version != 2) {
    f.close();
    return false;
  }

  uint32_t targetHash = static_cast<uint32_t>(std::hash<std::string>{}(epubPath));

  CompactIndexRecord rec;
  for (uint16_t i = 0; i < recordCount; i++) {
    f.seek(offsetOf(i));
    if (f.read((uint8_t*)&rec, sizeof(rec)) != sizeof(rec)) break;

    if (!(rec.flags & 1) && rec.cacheHash == targetHash) {
      rec.flags |= 1;
      f.seek(offsetOf(i));
      f.write((uint8_t*)&rec, sizeof(rec));
      f.close();
      return true;
    }
  }

  f.close();
  return false;
}

bool Ao3Librarian::setRecordFinished(const std::string& epubPath, bool finished) {
  const char* indexPath = "/.crosspoint/ao3_library_index.bin";
  if (!Storage.exists(indexPath)) return false;

  HalFile f = Storage.open(indexPath, O_RDWR);
  if (!f) return false;

  char magic[4];
  uint8_t version;
  uint16_t recordCount;
  bool readOk = f.read(magic, 4) == 4 && f.read(&version, 1) == 1 && f.read((uint8_t*)&recordCount, 2) == 2;

  if (!readOk || memcmp(magic, "AO3X", 4) != 0 || version != 2) {
    f.close();
    return false;
  }

  uint32_t targetHash = static_cast<uint32_t>(std::hash<std::string>{}(epubPath));

  CompactIndexRecord rec;
  for (uint16_t i = 0; i < recordCount; i++) {
    f.seek(offsetOf(i));
    if (f.read((uint8_t*)&rec, sizeof(rec)) != sizeof(rec)) break;

    if (!(rec.flags & 0x01) && rec.cacheHash == targetHash) {
      if (finished) {
        rec.flags |= 0x02;
      } else {
        rec.flags &= ~0x02;
      }
      f.seek(offsetOf(i));
      f.write((uint8_t*)&rec, sizeof(rec));
      f.close();
      return true;
    }
  }

  f.close();
  return false;
}

BookStatus Ao3Librarian::getBookStatus(const std::string& cachePath) {
  HalFile f;
  if (Storage.openFileForRead("AO3L", cachePath + "/ao3-status.bin", f)) {
    uint8_t status = 0;
    const bool ok = f.read(&status, 1) == 1;
    f.close();
    if (ok) return static_cast<BookStatus>(status);
  }
  return BookStatus::START;
}

void Ao3Librarian::saveBookStatus(const std::string& cachePath, const BookStatus status) {
  HalFile f;
  if (Storage.openFileForWrite("AO3L", cachePath + "/ao3-status.bin", f)) {
    const uint8_t data = static_cast<uint8_t>(status);
    f.write(&data, 1);
    f.close();
  }
}

int Ao3Librarian::sanitizeIndex() {
  const char* indexPath = "/.crosspoint/ao3_library_index.bin";
  if (!Storage.exists(indexPath)) return 0;

  HalFile f = Storage.open(indexPath, O_RDWR);
  if (!f) return -1;

  char magic[4];
  uint8_t version;
  uint16_t recordCount;
  bool readOk = f.read(magic, 4) == 4 && f.read(&version, 1) == 1 && f.read((uint8_t*)&recordCount, 2) == 2;

  if (!readOk || memcmp(magic, "AO3X", 4) != 0 || version != 2 || recordCount > MAX_LIBRARY_BOOKS) {
    f.close();
    return -1;
  }

  // Byte offset of the filepath field inside Ao3LibraryMetadata:
  // magic[4] + version[1] + rating[1] + warning[1] + isCompleted[1]
  // + wordCount[4] + chapterCount[2] + seriesPart[2]
  // + seriesName[128] + summary[512] + tags[4][16] = 720
  static constexpr size_t kFilepathOffset = 720;
  static constexpr size_t kFilepathLen = 256;

  int tombstoned = 0;
  CompactIndexRecord rec;

  for (uint16_t i = 0; i < recordCount; i++) {
    f.seek(offsetOf(i));
    if (f.read((uint8_t*)&rec, sizeof(rec)) != sizeof(rec)) break;

    if (rec.flags & 0x01) continue;  // already tombstoned

    // Check whether the sidecar exists for this hash.
    std::string sidecarPath = "/.crosspoint/epub_";
    sidecarPath += std::to_string(rec.cacheHash);
    sidecarPath += "/ao3_library_info";

    bool valid = false;
    if (Storage.exists(sidecarPath.c_str())) {
      // Read only the filepath field — avoids a full 1232-byte struct on the stack.
      HalFile sf;
      if (Storage.openFileForRead("AO3L", sidecarPath, sf)) {
        if (sf.seek(kFilepathOffset)) {
          char filepath[kFilepathLen];
          memset(filepath, 0, sizeof(filepath));
          sf.read((uint8_t*)filepath, kFilepathLen - 1);
          if (filepath[0] != '\0' && Storage.exists(filepath)) {
            valid = true;
          }
        }
        sf.close();
      }
    }

    if (!valid) {
      rec.flags |= 0x01;
      f.seek(offsetOf(i));
      f.write((uint8_t*)&rec, sizeof(rec));
      tombstoned++;
      LOG_DBG("AO3L", "sanitizeIndex: tombstoned ghost record (hash %u)", rec.cacheHash);
    }

    yield();
  }

  f.close();
  if (tombstoned > 0) {
    LOG_INF("AO3L", "sanitizeIndex: removed %d ghost record(s)", tombstoned);
  }
  return tombstoned;
}