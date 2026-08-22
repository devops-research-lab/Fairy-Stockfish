/*
  Fairy-Stockfish, a UCI chess variant playing engine derived from Stockfish
  Copyright (C) 2018-2022 Fabian Fichter

  Fairy-Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Fairy-Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "description.h"
#include "piece.h"
#include "position.h"
#include "thread.h"

namespace Stockfish {

namespace {

  // Helpers for assembling the human-readable rule description of a variant

  std::string result_str(Value value) {
      return  value ==  VALUE_MATE ? "win"
            : value == -VALUE_MATE ? "loss"
                                   : "draw";
  }

  std::string square_str(Square s) {
      return std::string(1, char('a' + file_of(s))) + std::to_string(rank_of(s) + 1);
  }

  // Compress a sorted list of indices into ranges, e.g., "1-3 and 8"
  std::string range_str(const std::vector<int>& items, std::function<std::string(int)> label) {
      std::string s;
      for (size_t i = 0; i < items.size(); )
      {
          size_t j = i;
          while (j + 1 < items.size() && items[j + 1] == items[j] + 1)
              j++;
          if (!s.empty())
              s += i + 1 == items.size() || j + 1 == items.size() ? " and " : ", ";
          s += label(items[i]);
          if (j > i)
              s += "-" + label(items[j]);
          i = j + 1;
      }
      return s;
  }

  // Describe a set of squares compactly, preferring full ranks/files over square lists
  std::string region_str(const Variant* v, Bitboard b) {
      Bitboard board = board_size_bb(v->maxFile, v->maxRank);
      b &= board;
      if (!b)
          return "no squares";
      if (b == board)
          return "the whole board";
      if (popcount(board & ~b) <= 3)
      {
          std::string s;
          for (Bitboard rest = board & ~b; rest; )
              s += (s.empty() ? "" : " ") + square_str(pop_lsb(rest));
          return "the whole board except " + s;
      }

      Bitboard covered = 0;
      std::vector<int> ranks;
      for (Rank r = RANK_1; r <= v->maxRank; ++r)
          if ((b & rank_bb(r)) == (rank_bb(r) & board))
          {
              ranks.push_back(r);
              covered |= rank_bb(r) & board;
          }
      if (covered == b)
          return (ranks.size() > 1 ? "ranks " : "rank ")
                + range_str(ranks, [](int r) { return std::to_string(r + 1); });

      covered = 0;
      std::vector<int> files;
      for (File f = FILE_A; f <= v->maxFile; ++f)
          if ((b & file_bb(f)) == (file_bb(f) & board))
          {
              files.push_back(f);
              covered |= file_bb(f) & board;
          }
      if (covered == b)
          return (files.size() > 1 ? "files " : "file ")
                + range_str(files, [](int f) { return std::string(1, char('a' + f)); });

      std::string s;
      while (b)
      {
          if (!s.empty())
              s += " ";
          s += square_str(pop_lsb(b));
      }
      return "the squares " + s;
  }

  // Describe a pair of per-color regions, collapsed when they are identical
  std::string region_pair_str(const Variant* v, Bitboard w, Bitboard b) {
      Bitboard board = board_size_bb(v->maxFile, v->maxRank);
      if ((w & board) == (b & board))
          return region_str(v, w);
      return region_str(v, w) + " (White) and " + region_str(v, b) + " (Black)";
  }

  // Betza notation of a piece type, resolving custom pieces and custom kings
  std::string betza_str(const Variant* v, PieceType pt) {
      if (pt == KING)
          pt = v->kingType;
      if (is_custom(pt))
          return v->customPiece[pt - CUSTOM_PIECES];
      auto it = pieceMap.find(pt);
      return it != pieceMap.end() ? it->second->betza : "";
  }

  // Human-readable name of a piece type within a variant
  std::string piece_str(const Variant* v, PieceType pt) {
      if (pt == ALL_PIECES)
          return "piece of any type";
      if (!is_custom(pt))
      {
          auto it = pieceMap.find(pt);
          if (it != pieceMap.end() && !it->second->name.empty())
              return it->second->name;
      }
      char c = v->pieceToChar[make_piece(WHITE, pt)];
      return std::string("custom piece '") + c + "'";
  }

  // List the piece types of a set, e.g., "knight, bishop, and rook"
  std::string piece_set_str(const Variant* v, PieceSet ps) {
      if (ps & ALL_PIECES)
          return "piece of any type";
      ps &= v->pieceTypes;
      std::vector<std::string> names;
      while (ps)
          names.push_back(piece_str(v, pop_lsb(ps)));
      std::string s;
      for (size_t i = 0; i < names.size(); i++)
          s += (i == 0 ? "" : i + 1 == names.size() ? " and " : ", ") + names[i];
      return s;
  }

  std::string castling_rights_str(CastlingRights cr) {
      std::vector<std::string> parts;
      if (cr & WHITE_OO)  parts.push_back("White kingside");
      if (cr & WHITE_OOO) parts.push_back("White queenside");
      if (cr & BLACK_OO)  parts.push_back("Black kingside");
      if (cr & BLACK_OOO) parts.push_back("Black queenside");
      std::string s;
      for (size_t i = 0; i < parts.size(); i++)
          s += (i == 0 ? "" : i + 1 == parts.size() ? " and " : ", ") + parts[i];
      return s;
  }

  // A titled group of rule statements that is only printed when non-empty
  struct Section {
      std::string title;
      std::vector<std::string> items;
      explicit Section(const std::string& t) : title(t) {}
      void add(const std::string& s) { items.push_back(s); }
  };

} // namespace

// describe_variant() generates a human-readable description of the rules of a
// variant from its configuration, e.g., for the "rules" UCI/XBoard command.
// Rules matching standard chess are generally not spelled out explicitly.

std::string describe_variant(const std::string& name, const Variant* v) {

    std::ostringstream os;
    std::string title = name;
    title[0] = toupper(title[0]);

    // Overview
    os << title << " is a chess variant played on a board of "
       << v->maxFile + 1 << "x" << v->maxRank + 1 << " squares with "
       << popcount(v->pieceTypes) << " piece type" << (popcount(v->pieceTypes) > 1 ? "s" : "") << "." << std::endl;
    if (v->chess960)
        os << "The starting position is randomized with Chess960 castling rules." << std::endl;
    if (v->twoBoards)
        os << "It is played on two boards: pieces captured on one board enter the pocket of the partner on the other board." << std::endl;

    os << std::endl << "Starting position (FEN): " << v->startFen << std::endl;
    {
        StateInfo st;
        Position pos;
        pos.set(v, v->startFen, v->chess960, &st, Threads.main());
        print_board_diagram(os, pos);
    }

    // Piece list with movement in Betza notation
    os << std::endl << "# Pieces" << std::endl << std::endl;
    for (PieceSet ps = v->pieceTypes; ps;)
    {
        PieceType pt = pop_lsb(ps);
        os << "  " << v->pieceToChar[make_piece(WHITE, pt)] << ": " << piece_str(v, pt);
        std::string betza = betza_str(v, pt);
        if (!betza.empty())
            os << " (Betza: " << betza << ")";
        if (pt == KING)
            os << " - royal piece";
        os << std::endl;
    }

    std::vector<Section> sections;

    // Board and movement rules
    Section movement("Movement");

    if (v->pieceTypes & PAWN)
    {
        if (!v->doubleStep)
            movement.add("Pawns may not make an initial double step.");
        else if (   (v->doubleStepRegion[WHITE] & board_size_bb(v->maxFile, v->maxRank)) != (Rank2BB & board_size_bb(v->maxFile, v->maxRank))
                 || (v->doubleStepRegion[BLACK] & board_size_bb(v->maxFile, v->maxRank)) != (rank_bb(Rank(v->maxRank - 1)) & board_size_bb(v->maxFile, v->maxRank)))
            movement.add("Pawns may make a double step from "
                        + region_pair_str(v, v->doubleStepRegion[WHITE], v->doubleStepRegion[BLACK]) + ".");
        if (v->tripleStepRegion[WHITE] || v->tripleStepRegion[BLACK])
            movement.add("Pawns may even make a triple step from "
                        + region_pair_str(v, v->tripleStepRegion[WHITE], v->tripleStepRegion[BLACK]) + ".");
        if (v->doubleStep)
        {
            if (!(v->enPassantRegion[WHITE] | v->enPassantRegion[BLACK]))
                movement.add("There is no en passant capture.");
            else if (   v->enPassantRegion[WHITE] != AllSquares || v->enPassantRegion[BLACK] != AllSquares
                     || (v->enPassantTypes[WHITE] & v->pieceTypes) != (piece_set(PAWN) & v->pieceTypes)
                     || (v->enPassantTypes[BLACK] & v->pieceTypes) != (piece_set(PAWN) & v->pieceTypes))
            {
                std::string s = "En passant captures are allowed for the following piece types: "
                              + piece_set_str(v, v->enPassantTypes[WHITE] | v->enPassantTypes[BLACK]) + ".";
                if (v->enPassantRegion[WHITE] != AllSquares || v->enPassantRegion[BLACK] != AllSquares)
                    s += " They are restricted to "
                       + region_pair_str(v, v->enPassantRegion[WHITE], v->enPassantRegion[BLACK]) + ".";
                movement.add(s);
            }
        }
    }

    if (!v->castling)
        movement.add("There is no castling.");
    else
    {
        if (v->castlingKingPiece[WHITE] != KING || (v->castlingRookPieces[WHITE] & v->pieceTypes) != (piece_set(ROOK) & v->pieceTypes))
            movement.add("Castling is performed by the " + piece_str(v, v->castlingKingPiece[WHITE])
                        + " together with a piece of one of these types: " + piece_set_str(v, v->castlingRookPieces[WHITE]) + ".");
        if (v->castlingKingsideFile != FILE_G || v->castlingQueensideFile != FILE_C || v->castlingRank != RANK_1)
            movement.add(std::string("After castling, the king ends up on file ") + char('a' + v->castlingKingsideFile)
                        + " (kingside) or " + char('a' + v->castlingQueensideFile) + " (queenside) of rank "
                        + std::to_string(v->castlingRank + 1) + " (from each player's perspective).");
        if (v->oppositeCastling)
            movement.add("A player may not castle to the same side as the opponent.");
        if (v->castlingDroppedPiece)
            movement.add("Castling is also allowed with pieces that entered the game by being dropped.");
    }

    if (!v->checking)
        movement.add("Moves that give check are not allowed.");
    if (v->mustCapture)
        movement.add("Capturing is mandatory whenever a capture is available.");
    if (v->pass[WHITE] || v->pass[BLACK])
        movement.add(std::string(v->pass[WHITE] && v->pass[BLACK] ? "Both players" : v->pass[WHITE] ? "White" : "Black")
                    + " may pass the turn instead of moving.");
    if (v->passOnStalemate[WHITE] || v->passOnStalemate[BLACK])
        movement.add(std::string(v->passOnStalemate[WHITE] && v->passOnStalemate[BLACK] ? "A player" : v->passOnStalemate[WHITE] ? "White" : "Black")
                    + " without a legal move passes the turn instead of being stalemated.");
    if (v->makpongRule)
        movement.add("The king may not move to escape a check (makpong rule).");
    if (v->flyingGeneral)
        movement.add("The two kings may not directly face each other on an open file.");
    if (v->diagonalLines)
        movement.add("Moves along the diagonal palace lines are allowed between " + region_str(v, v->diagonalLines) + ".");
    if (v->cambodianMoves)
        movement.add("The king may make an initial knight jump and the met (fers) an initial "
                     "two-square forward jump, unless the king has already been checked (Cambodian rule).");
    if ((v->pieceTypes & SOLDIER) && v->soldierPromotionRank > RANK_1)
        movement.add("Soldiers move only straight forward and gain their sideways step upon reaching rank "
                    + std::to_string(v->soldierPromotionRank + 1) + " (from the player's perspective).");
    for (Color c : {WHITE, BLACK})
        for (PieceSet ps = v->pieceTypes; ps;)
        {
            PieceType pt = pop_lsb(ps);
            if (v->mobilityRegion[c][pt])
                movement.add("The " + std::string(c == WHITE ? "white " : "black ") + piece_str(v, pt)
                            + " may only move within " + region_str(v, v->mobilityRegion[c][pt]) + ".");
        }
    if (v->immobilityIllegal)
        movement.add("It is illegal to move or drop a piece to a square from which it could never move again.");
    if (v->mutuallyImmuneTypes)
        movement.add("The following piece types can not capture pieces of the same set: "
                    + piece_set_str(v, v->mutuallyImmuneTypes) + ".");
    if (v->blastOnCapture)
    {
        std::string s = "Every capture causes an explosion that destroys the capturing and captured pieces "
                        "as well as all non-pawn pieces on adjacent squares.";
        if (v->blastImmuneTypes & v->pieceTypes)
            s += " Immune to explosions: " + piece_set_str(v, v->blastImmuneTypes) + ".";
        movement.add(s);
    }
    if (v->petrifyOnCaptureTypes & v->pieceTypes)
    {
        std::string s = "The following piece types are petrified, i.e., turn into immovable wall squares, when they capture: "
                      + piece_set_str(v, v->petrifyOnCaptureTypes) + ".";
        if (v->blastOnCapture && v->petrifyBlastPieces)
            s += " Pieces destroyed in an explosion are petrified as well.";
        movement.add(s);
    }
    if (v->wallingRule)
    {
        std::string s =   v->wallingRule == ARROW ? "After moving, a piece shoots an arrow to a square it could move to, "
                                                    "which becomes an impassable wall square (as in Game of the Amazons)."
                        : v->wallingRule == DUCK  ? "After each move, the player must relocate the duck, a wall square that "
                                                    "blocks all movement, to any empty square (duck chess)."
                        : v->wallingRule == EDGE  ? "After each move, the player removes a square at an edge of the board, "
                                                    "which becomes an impassable wall (as in Atlantis)."
                        : v->wallingRule == PAST  ? "The square a piece moves from becomes an impassable wall square (as in Snailtrail)."
                        :                           "After each move, the player turns an empty square into an impassable wall square (as in Isolation).";
        if ((v->wallingRegion[WHITE] & board_size_bb(v->maxFile, v->maxRank)) != board_size_bb(v->maxFile, v->maxRank))
            s += " Walls are restricted to " + region_str(v, v->wallingRegion[WHITE]) + ".";
        if (v->wallOrMove)
            s = "A player either moves a piece or places a wall, but not both. " + s;
        movement.add(s);
    }
    sections.push_back(movement);

    // Piece drops, pockets, and gating
    Section drops("Drops and hand");

    if (v->pieceDrops)
    {
        if (v->freeDrops)
            drops.add("As a move, a player may place a new piece on an empty square, "
                      "drawing from an unlimited supply.");
        else if (v->capturesToHand)
            drops.add("Captured pieces change color, go into the capturing player's hand, "
                      "and can be dropped back onto the board on a later move.");
        else
            drops.add("Pieces in hand can be dropped onto the board as a move.");
        if (v->mustDrop)
            drops.add("Dropping "
                     + std::string(v->mustDropType == ALL_PIECES ? "a piece" : ("a " + piece_str(v, v->mustDropType)))
                     + " is mandatory while one is in hand.");
        if (v->dropLoop)
            drops.add("Captured promoted pieces stay promoted in hand instead of reverting to their base type.");
        if (!v->dropChecks)
            drops.add("A dropped piece may not give check.");
        if (v->pieceTypes & PAWN)
        {
            if (!v->firstRankPawnDrops)
                drops.add("Pawns may not be dropped on a player's first rank.");
            if (!v->promotionZonePawnDrops)
                drops.add("Pawns may not be dropped inside the promotion zone.");
        }
        if (v->dropRegion[WHITE] != AllSquares || v->dropRegion[BLACK] != AllSquares)
            drops.add("Drops are restricted to "
                     + region_pair_str(v, v->dropRegion[WHITE], v->dropRegion[BLACK]) + ".");
        if (v->sittuyinRookDrop)
            drops.add("Rooks may only be dropped on the player's first rank.");
        if (v->dropOppositeColoredBishop)
            drops.add("Bishops may only be dropped on square colors on which the player does not have a bishop yet.");
        if (v->dropPromoted)
            drops.add("Pieces may be dropped in their promoted state.");
        if (v->dropNoDoubled != NO_PIECE_TYPE)
            drops.add("A " + piece_str(v, v->dropNoDoubled) + " may not be dropped on a file already containing "
                     + (v->dropNoDoubledCount == 1 ? "an unpromoted " + piece_str(v, v->dropNoDoubled)
                                                   : std::to_string(v->dropNoDoubledCount) + " unpromoted "
                                                     + piece_str(v, v->dropNoDoubled) + "s") + " of the same player.");
        if (v->enclosingDrop)
            drops.add(  v->enclosingDrop == REVERSI     ? "A drop must enclose at least one line of opponent pieces between "
                                                          "the dropped piece and another own piece (as in Reversi)."
                      : v->enclosingDrop == ATAXX       ? "A drop must be made on a square adjacent to an own piece (as in Ataxx)."
                      : v->enclosingDrop == QUADWRANGLE ? "A drop must be adjacent to an own piece (Quadwrangle rule)."
                      : v->enclosingDrop == SNORT       ? "A drop must not be made adjacent (orthogonally) to an opponent piece (as in Snort)."
                      : v->enclosingDrop == ANYSIDE     ? "Pieces are inserted from any edge of the board and slide to the opposite edge."
                      :                                   "Pieces are dropped from the top of a file and fall to its lowest empty square (as in Connect Four)."
                     );
        if (v->enclosingDrop && v->enclosingDropStart)
            drops.add("During the initial phase, drops are instead allowed on " + region_str(v, v->enclosingDropStart) + ".");
    }
    if (v->flipEnclosedPieces)
        drops.add(  v->flipEnclosedPieces == REVERSI ? "All opponent pieces enclosed in a straight line between the newly placed "
                                                       "piece and another own piece change color (as in Reversi)."
                  : v->flipEnclosedPieces == ATAXX   ? "All opponent pieces adjacent to the newly placed piece change color (as in Ataxx)."
                  :                                    "After a normal move or a drop next to an own piece, all adjacent opponent "
                                                       "pieces change color (Quadwrangle rule).");
    if (v->gating)
    {
        if (v->seirawanGating)
            drops.add("When a piece moves from its starting back-rank square for the first time, a piece in hand "
                      "may be gated onto the vacated square as part of the move (as in S-Chess).");
        else if (!v->cambodianMoves)
            drops.add("Some squares carry special rights, tracked in the castling field of the FEN (gating).");
    }
    sections.push_back(drops);

    // Promotion rules
    Section promotion("Promotion");

    Bitboard board = board_size_bb(v->maxFile, v->maxRank);
    bool zoneEverywhere =   (v->promotionRegion[WHITE] & board) == board
                         && (v->promotionRegion[BLACK] & board) == board;
    std::string zone =   (v->promotionRegion[WHITE] & board) == (rank_bb(v->maxRank) & board)
                      && (v->promotionRegion[BLACK] & board) == (Rank1BB & board)
                      ? "the last rank"
                      : region_pair_str(v, v->promotionRegion[WHITE], v->promotionRegion[BLACK]);
    PieceSet pawnTypes = (v->promotionPawnTypes[WHITE] | v->promotionPawnTypes[BLACK]) & v->pieceTypes;
    PieceSet promotionTypes = (v->promotionPieceTypes[WHITE] | v->promotionPieceTypes[BLACK]) & v->pieceTypes;
    if (pawnTypes && promotionTypes)
    {
        std::string mover = pawnTypes == (piece_set(PAWN) & v->pieceTypes) ? "Pawns"
                          : "The following piece types promote like pawns: " + piece_set_str(v, pawnTypes) + ". They";
        promotion.add(mover + (zoneEverywhere ? " may promote with any move" : " promote in " + zone)
                     + " to: " + piece_set_str(v, promotionTypes) + ".");
        if (!v->mandatoryPawnPromotion)
            promotion.add("Pawn promotion is optional.");
        if (v->sittuyinPromotion)
            promotion.add("Promotion is a move in itself: an eligible pawn promotes either in place "
                          "or with a non-capturing move of the promoted piece (Sittuyin rule).");
        std::string limits;
        for (PieceSet ps = promotionTypes; ps;)
        {
            PieceType pt = pop_lsb(ps);
            if (v->promotionLimit[pt])
                limits += (limits.empty() ? "" : ", ") + std::to_string(v->promotionLimit[pt])
                        + " " + piece_str(v, pt) + (v->promotionLimit[pt] > 1 ? "s" : "");
        }
        if (!limits.empty())
            promotion.add("Promotion is limited by the total number of pieces a player may have of a type: "
                         "at most " + limits + ".");
    }

    std::string promotedMap;
    for (PieceSet ps = v->pieceTypes; ps;)
    {
        PieceType pt = pop_lsb(ps);
        if (v->promotedPieceType[pt] != NO_PIECE_TYPE)
            promotedMap += (promotedMap.empty() ? "" : ", ") + piece_str(v, pt) + " to "
                         + piece_str(v, v->promotedPieceType[pt]);
    }
    if (!promotedMap.empty())
    {
        promotion.add((zoneEverywhere ? "Pieces may promote with any move as follows: "
                                      : "Pieces moving within the promotion zone, i.e., " + zone + ", may promote as follows: ")
                     + promotedMap + ".");
        if (v->mandatoryPiecePromotion)
            promotion.add("Piece promotion is mandatory.");
        if (v->piecePromotionOnCapture)
            promotion.add("Pieces may only promote on captures.");
        if (v->pieceDemotion)
            promotion.add("Promoted pieces may also demote back to their base type.");
    }
    sections.push_back(promotion);

    // Game end and adjudication rules
    Section end("Game end");

    if (v->checking && ((v->pieceTypes & KING) || v->extinctionPseudoRoyal))
    {
        end.add("Checkmate is a " + result_str(v->checkmateValue) + " for the checkmated player.");
        if (v->shogiPawnDropMateIllegal)
            end.add("Giving checkmate by a pawn drop is illegal.");
        if (v->shatarMateRule)
            end.add("Checkmating with a knight is illegal, and a mating sequence of checks "
                    "must contain a check by rook or bers (shak) to win (shatar rules).");
    }
    end.add("Stalemate is a " + result_str(v->stalemateValue) + " for the player without a legal move.");
    if (v->stalematePieceCount)
        end.add("In case of stalemate, the player with more pieces on the board wins instead.");

    if (v->extinctionValue != VALUE_NONE)
    {
        bool anyPiece = bool(v->extinctionPieceTypes & ALL_PIECES);
        bool singleType = !anyPiece && popcount(v->extinctionPieceTypes & v->pieceTypes) == 1;
        std::string what = anyPiece    ? std::string("pieces")
                         : singleType  ? piece_set_str(v, v->extinctionPieceTypes) + "s"
                                       : "pieces of any one type (" + piece_set_str(v, v->extinctionPieceTypes) + ")";
        std::string subject =
              v->extinctionPieceCount == 0
            ? "A player who runs out of " + what
            : v->extinctionPieceCount == 1 && anyPiece
            ? "A player who is reduced to a single piece"
            : "A player who is reduced to at most " + std::to_string(v->extinctionPieceCount) + " " + what;
        std::string outcome = v->extinctionValue ==  VALUE_MATE ? "wins the game."
                            : v->extinctionValue == -VALUE_MATE ? "loses the game."
                                                                : "brings the game to a draw.";
        std::string s = subject + " " + outcome;
        if (v->extinctionOpponentPieceCount)
            s += " This only applies while the opponent has at least "
               + std::to_string(v->extinctionOpponentPieceCount) + " of them.";
        if (v->extinctionClaim)
            s += " It has to be claimed by the player to move.";
        end.add(s);
        if (v->extinctionPseudoRoyal)
            end.add(std::string("The pieces subject to the extinction rule are royal: they may not be left en prise.")
                   + (v->dupleCheck ? " It only counts as a check if all of them are attacked at the same time." : ""));
    }

    if (v->flagRegion[WHITE] || v->flagRegion[BLACK])
    {
        std::string s;
        if (   v->flagRegion[WHITE] && v->flagRegion[BLACK]
            && v->flagPiece[WHITE] == v->flagPiece[BLACK])
        {
            std::string piece = piece_str(v, v->flagPiece[WHITE]);
            std::string target = v->flagRegion[WHITE] == v->flagRegion[BLACK]
                               ? region_str(v, v->flagRegion[WHITE])
                               : region_str(v, v->flagRegion[WHITE]) + " (White) or "
                                 + region_str(v, v->flagRegion[BLACK]) + " (Black)";
            s = "A player wins by moving " + std::string(v->flagPieceCount > 1 ? std::to_string(v->flagPieceCount) + " of their " + piece + "s"
                                                                               : "their " + piece)
              + " to " + target + ".";
        }
        else
            for (Color c : {WHITE, BLACK})
                if (v->flagRegion[c])
                    s += std::string(s.empty() ? "" : " ") + (c == WHITE ? "White" : "Black") + " wins by moving "
                       + (v->flagPieceCount > 1 ? std::to_string(v->flagPieceCount) + " of their " + piece_str(v, v->flagPiece[c]) + "s"
                                                : "their " + piece_str(v, v->flagPiece[c]))
                       + " to " + region_str(v, v->flagRegion[c]) + ".";
        if (v->flagPieceCount > 1 && v->flagPieceBlockedWin)
            s += " It also counts if the remaining target squares are blocked by other pieces.";
        end.add(s);
        if (v->flagPieceSafe)
            end.add("The pieces reaching the target region must not be attacked for the win to count.");
        if (v->flagMove)
            end.add("When White reaches the target region first, Black gets one more move to reach it as well, "
                    "in which case the game is a draw.");
    }

    if (v->checkCounting)
        end.add("A player wins by giving the total number of checks specified in the FEN, "
                "e.g., three checks in Three-check.");

    std::vector<std::string> dirs;
    if (v->connectHorizontal)
        dirs.push_back("horizontal");
    if (v->connectVertical)
        dirs.push_back("vertical");
    if (v->connectDiagonal)
        dirs.push_back("diagonal");
    std::string connectDirs;
    for (size_t i = 0; i < dirs.size(); i++)
        connectDirs += (i == 0 ? "" : i + 1 == dirs.size() ? " or " : ", ") + dirs[i];
    std::string connectPieces = (v->connectPieceTypesTrimmed & v->pieceTypes) == v->pieceTypes
                              ? "pieces" : piece_set_str(v, v->connectPieceTypesTrimmed) + " pieces";
    if (v->connectN)
        end.add("Forming an unbroken " + connectDirs + " line of " + std::to_string(v->connectN)
               + " of one's own " + connectPieces + " is a " + result_str(v->connectValue) + " for that player.");
    if (v->connectRegion1[WHITE] || v->connectRegion1[BLACK])
        end.add("Connecting the two opposite target regions with an unbroken (" + connectDirs + ") chain of one's own "
               + connectPieces + " is a " + result_str(v->connectValue) + " for that player "
               "(White connects " + region_str(v, v->connectRegion1[WHITE]) + " with " + region_str(v, v->connectRegion2[WHITE])
               + ", Black " + region_str(v, v->connectRegion1[BLACK]) + " with " + region_str(v, v->connectRegion2[BLACK]) + ").");
    if (v->connectNxN)
        end.add("Forming a filled " + std::to_string(v->connectNxN) + "x" + std::to_string(v->connectNxN)
               + " square of one's own " + connectPieces + " is a " + result_str(v->connectValue) + " for that player.");
    if (v->collinearN)
        end.add("Having " + std::to_string(v->collinearN) + " of one's own " + connectPieces
               + " on a common line, not necessarily adjacent, is a " + result_str(v->connectValue) + " for that player.");

    if (v->castlingWins)
        end.add("Performing the following castling moves wins the game, and losing the respective "
                "castling rights loses: " + castling_rights_str(v->castlingWins) + ".");

    if (v->bikjangRule)
        end.add("When the two kings directly face each other on an open file (bikjang) and the opponent "
                "does not resolve it, the game is adjudicated as a draw.");

    if (v->nMoveRule)
        end.add("The game is drawn after " + std::to_string(v->nMoveRule) + " consecutive moves by each player without a "
               + (v->nMoveRuleTypes[WHITE] & v->pieceTypes ? piece_set_str(v, v->nMoveRuleTypes[WHITE]) + " move or " : "")
               + "capture.");
    if (v->nFoldRule)
    {
        std::string s = "A position repeating " + std::to_string(v->nFoldRule) + " times is a "
                      + result_str(v->nFoldValue);
        if (v->nFoldValue != VALUE_DRAW)
            s += v->nFoldValueAbsolute ? " for White" : " for the player to move";
        end.add(s + ".");
        if (v->perpetualCheckIllegal)
            end.add("Perpetual check is illegal: on repetition, the result is instead a loss for a player "
                    "who checked on all of their moves in between.");
        if (v->moveRepetitionIllegal)
            end.add("Moving the same piece back and forth between the same two squares "
                   + std::to_string(v->nFoldRule - 1) + " times in a row is not allowed.");
        if (v->chasingRule)
            end.add("Perpetually chasing an unprotected piece is forbidden and adjudicated "
                    "according to the Asian Xiangqi Federation rules.");
    }

    if (v->countingRule)
        end.add(  std::string(  v->countingRule == MAKRUK_COUNTING    ? "Makruk"
                              : v->countingRule == CAMBODIAN_COUNTING ? "Cambodian"
                                                                      : "ASEAN")
                + " counting rules apply: in the endgame, the disadvantaged side counts moves "
                  "and the game is drawn when a material-dependent limit is reached before the win.");
    if (v->materialCounting)
        end.add(  v->materialCounting == JANGGI_MATERIAL     ? "When the game is adjudicated, the winner is decided by counting material points (Janggi values)."
                : v->materialCounting == UNWEIGHTED_MATERIAL ? "When the game is adjudicated, the player with more pieces on the board wins."
                : v->materialCounting == WHITE_DRAW_ODDS     ? "Games without any other result are scored as a win for White (draw odds)."
                                                             : "Games without any other result are scored as a win for Black (draw odds).");
    if (v->adjudicateFullBoard)
        end.add("When the board is completely filled, the game ends and is adjudicated by material.");

    sections.push_back(end);

    for (const Section& section : sections)
    {
        if (section.items.empty())
            continue;
        os << std::endl << "# " << section.title << std::endl << std::endl;
        for (const std::string& item : section.items)
            os << "  * " << item << std::endl;
    }

    return os.str();
}

} // namespace Stockfish
