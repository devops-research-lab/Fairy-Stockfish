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

#ifndef DESCRIPTION_H_INCLUDED
#define DESCRIPTION_H_INCLUDED

#include <string>

#include "variant.h"

namespace Stockfish {

// Generate a human-readable description of the rules of a variant
// from its configuration, e.g., for the "rules" UCI/XBoard command.
std::string describe_variant(const std::string& name, const Variant* v);

} // namespace Stockfish

#endif // #ifndef DESCRIPTION_H_INCLUDED
