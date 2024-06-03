/*******************************************************************************

  Copyright (c) Honda Research Institute Europe GmbH.
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.

  3. Neither the name of the copyright holder nor the names of its
     contributors may be used to endorse or promote products derived from
     this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER "AS IS" AND ANY EXPRESS OR
  IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
  OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
  IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY DIRECT, INDIRECT,
  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
  OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
  EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*******************************************************************************/

#include "ActionResult.h"

#include <Rcs_macros.h>


namespace aff
{

std::string ActionResult::toString() const
{
  std::string res;

  if (success())
  {
    res = "ACTION: '" + actionCommand + "' : SUCCESS";
  }
  else
  {
    res = "ACTION: '" + actionCommand + "' ERROR: '" + error + "' REASON: '" + reason
          + "' SUGGESTION: '" + suggestion + "' DEVELOPER: '" + developer + "'";
  }

  return res;
}

std::vector<std::string> ActionResult::toStringVec() const
{
  std::vector<std::string> res(5);

  res[0] = error;
  res[1] = actionCommand;
  res[2] = reason;
  res[3] = suggestion;
  res[4] = developer;

  return res;
}

void ActionResult::clear()
{
  error.clear();
  reason.clear();
  suggestion.clear();
  developer.clear();
}

bool ActionResult::success() const
{
  return STRNEQ(error.c_str(), "SUCCESS", 7);
}

}   // namespace aff
