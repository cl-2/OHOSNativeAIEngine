/*
 * MessageBubble.ets – 对话气泡组件
 * 用户消息：右对齐、蓝色背景、白色文字
 * AI 消息：左对齐、白色背景、深色文字 + AI 头像
 */

const PRIMARY_COLOR: string = '#4F46E5';

@Component
export default struct MessageBubble {
  @Prop msg: string = '';
  @Prop from: string = 'user';
  speakClick?: () => void;

  build() {
    Row() {
      // AI 消息 -> 左侧头像
      if (this.from === 'assistant') {
        Text('AI')
          .fontSize(13).fontColor('#FFFFFF').fontWeight('700')
          .width(36).height(36).textAlign(TextAlign.Center)
          .background('linear-gradient(135deg, #6366F1, #8B5CF6)')
          .borderRadius(18)
          .margin({ right: 8 })
      }

      Column() {
        // 气泡内容
        Text(this.msg)
          .fontSize(15)
          .fontColor(this.from === 'user' ? '#FFFFFF' : '#1F2937')
          .lineHeight(22)
          .padding({ left: 16, right: 16, top: 12, bottom: 12 })
          .backgroundColor(this.from === 'user' ? PRIMARY_COLOR : '#FFFFFF')
          .borderRadius(this.from === 'user' ?
            { topLeft: 18, topRight: 18, bottomLeft: 18, bottomRight: 4 } :
            { topLeft: 18, topRight: 18, bottomLeft: 4, bottomRight: 18 })
          .shadow({ radius: 4, offsetX: 0, offsetY: 1, color: 'rgba(0,0,0,0.05)' })
          .maxLines(100)
          .textOverflow({ overflow: TextOverflow.None })

        // AI 消息 -> 朗读按钮
        if (this.from === 'assistant') {
          Row() {
            Button() {
              Text('🔊 朗读').fontSize(11).fontColor('#6B7280')
            }
            .height(26)
            .backgroundColor('#F3F4F6')
            .borderRadius(13)
            .padding({ left: 10, right: 10 })
            .margin({ top: 4 })
            .onClick((): void => {
              if (this.speakClick) this.speakClick();
            })
          }
          .width('100%')
          .justifyContent(FlexAlign.Start)
        }
      }

      // 用户消息 -> 右侧头像
      if (this.from === 'user') {
        Text('我')
          .fontSize(13).fontColor('#FFFFFF').fontWeight('700')
          .width(36).height(36).textAlign(TextAlign.Center)
          .background('linear-gradient(135deg, #3B82F6, #6366F1)')
          .borderRadius(18)
          .margin({ left: 8 })
      }
    }
    .width('100%')
    .justifyContent(this.from === 'user' ? FlexAlign.End : FlexAlign.Start)
    .margin({ top: 10 })
    .padding({ left: this.from === 'assistant' ? 0 : 48, right: this.from === 'user' ? 0 : 48 })
  }
}