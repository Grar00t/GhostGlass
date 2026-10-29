using System.Text.Json;
namespace GhostGlass.Host;
internal sealed record ChatTurn(string Role,string Content);
internal sealed class ConversationState {
 public string ConversationId {get;}=Guid.NewGuid().ToString("N");
 private readonly List<ChatTurn> turns=new();
 public IReadOnlyList<ChatTurn> Turns=>turns;
 public void AppendUser(string content){if(string.IsNullOrWhiteSpace(content))throw new ArgumentException("empty");turns.Add(new("user",content));Trim();}
 public void CompleteAssistant(string content){if(!string.IsNullOrWhiteSpace(content)){turns.Add(new("assistant",content));Trim();}}
 private void Trim(){while(turns.Count>20)turns.RemoveAt(0);}
 public List<ChatTurn> Payload(){
  var result=new List<ChatTurn>{new("system","You are an assistant within GhostGlass. Answer the user's actual request, including follow-ups, using only the information provided. Do not claim screen access or tool capabilities you do not have. Respond in the user's language.")};
  result.AddRange(turns);
  return result;
 }
}
internal static class ConversationPayload {
 public static IReadOnlyList<ChatTurn> Validate(IReadOnlyList<ChatTurn> turns){
  if(turns.Count<1||turns.Count>21)throw new InvalidDataException("invalid_history_length");
  int chars=0;foreach(var turn in turns){
   if(turn.Role is not ("system" or "user" or "assistant"))throw new InvalidDataException("invalid_role");
   chars+=turn.Content.Length;
   if(chars>24000)throw new InvalidDataException("history_too_large");
  }
  if(turns[^1].Role!="user")throw new InvalidDataException("last_turn_not_user");
  return turns;
 }
}
