/*
 * MessageBubble.ets – “自然呼吸”主题对话卡片
 * 只负责展示；朗读回调和消息数据结构保持不变。
 */

const MINT: string = '#45AFA8';
const MINT_DARK: string = '#278C87';
const MINT_LIGHT: string = '#EAF7F4';

@Component
export default struct MessageBubble {
  @Prop msg: string = '';
  @Prop from: string = 'user';
  @Prop source: string = '';
  speakClick?: () => void;

  build(): void {
    Row() {
      if (this.from === 'assistant') {
        Stack({ alignContent: Alignment.Center }) {
          Circle().width(36).height(36).fill('#FFFFFF')
            .stroke(MINT_LIGHT).strokeWidth(4)
            .shadow({ radius: 8, offsetX: 0, offsetY: 3, color: 'rgba(39,140,135,0.14)' })
          Circle().width(14).height(14).fill(MINT)
          Circle().width(5).height(5).fill('#D9F5EF')
        }
        .width(40).height(40)
        .margin({ right: 8, top: 2 })
      }

      Column() {
        Text(this.msg)
          .fontSize(15)
          .fontColor(this.from === 'user' ? '#214943' : '#26332F')
          .lineHeight(23)
          .padding({ left: 16, right: 16, top: 13, bottom: 13 })
          .backgroundColor(this.from === 'user' ? '#DDF2ED' : '#FFFFFF')
          .borderRadius(this.from === 'user' ?
            { topLeft: 18, topRight: 18, bottomLeft: 18, bottomRight: 6 } :
            { topLeft: 18, topRight: 18, bottomLeft: 6, bottomRight: 18 })
          .border({
            width: this.from === 'user' ? 0 : 1,
            color: this.from === 'user' ? 'transparent' : '#E8EFEC'
          })
          .shadow({ radius: 10, offsetX: 0, offsetY: 3, color: 'rgba(37,70,63,0.07)' })
          .maxLines(100)
          .textOverflow({ overflow: TextOverflow.None })

        if (this.from === 'assistant') {
          Row({ space: 8 }) {
            if (this.source) {
              Row({ space: 4 }) {
                Circle().width(5).height(5).fill(MINT)
                Text(this.source).fontSize(10).fontColor('#78908A').maxLines(1)
              }
              .padding({ left: 9, right: 9, top: 5, bottom: 5 })
              .backgroundColor('#F2F8F6')
              .borderRadius(12)
            }
            Button() {
              Row({ space: 4 }) {
                Text('▷').fontSize(10).fontColor(MINT_DARK)
                Text('朗读').fontSize(11).fontColor(MINT_DARK)
              }
            }
            .height(26)
            .backgroundColor(MINT_LIGHT)
            .borderRadius(13)
            .padding({ left: 10, right: 10 })
            .onClick((): void => {
              if (this.speakClick) this.speakClick();
            })
          }
          .width('100%')
          .margin({ top: 6, left: 2 })
          .justifyContent(FlexAlign.Start)
        }
      }
      .alignItems(this.from === 'user' ? HorizontalAlign.End : HorizontalAlign.Start)

      if (this.from === 'user') {
        Circle()
          .width(30).height(30)
          .fill('#F0F7F5')
          .stroke('#D8EAE6').strokeWidth(1)
          .overlay('我', { align: Alignment.Center })
          .margin({ left: 8, top: 2 })
      }
    }
    .width('100%')
    .alignItems(VerticalAlign.Top)
    .justifyContent(this.from === 'user' ? FlexAlign.End : FlexAlign.Start)
    .margin({ top: 12 })
    .padding({ left: this.from === 'assistant' ? 0 : 50, right: this.from === 'user' ? 0 : 38 })
  }
}
